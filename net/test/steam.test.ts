// Steam sign-in, which is not OAuth at all.
//
// The other seven providers speak OAuth 2.0 in one dialect or another. Steam
// speaks OpenID 2.0: no client registration, no secret, no scopes, no tokens.
// The identity arrives on the callback ALREADY SIGNED, which sounds stronger
// than a code flow and is in fact weaker in one specific way -- every parameter
// was written by whoever loaded the URL. A `claimed_id` naming any SteamID64 in
// the world costs an attacker nothing to type.
//
// So three things have to hold, and all three are tested here:
//
//   1. Nothing in the query is believed until Steam has confirmed the signature
//      is theirs, via openid.mode=check_authentication.
//   2. The state -- which OpenID 2.0 has no parameter for -- rides in return_to,
//      and the echoed return_to is checked against the one we would have built.
//   3. Steam cannot CREATE an account, only link to one. Its creation date is
//      published only for public profiles, so it goes missing exactly when
//      somebody has a reason to hide it.

import { beforeEach, describe, expect, it, vi } from "vitest";
import { clientCredentials, PROVIDERS, PROVIDER_IDS, isProviderId } from "../src/auth/providers.js";
import { authorizeUrl, identityFromOpenId } from "../src/auth/oauth.js";

beforeEach(() => { vi.restoreAllMocks(); });

const ENV = { ISSUER: "https://accounts.example.test" } as never;
const CLAIMS = { rid: "r1", provider: "steam", purpose: "login" } as never;
const STEAMID = "76561198000000001";
const CLAIMED = `https://steamcommunity.com/openid/id/${STEAMID}`;
const RETURN_TO = "https://accounts.example.test/auth/callback/steam?state=st";

/** A callback URL shaped the way Steam sends one. */
function callback(over: Record<string, string> = {}): URL {
    const u = new URL("https://accounts.example.test/auth/callback/steam");
    const params: Record<string, string> = {
        state: "st",
        "openid.mode": "id_res",
        "openid.return_to": RETURN_TO,
        "openid.claimed_id": CLAIMED,
        "openid.identity": CLAIMED,
        "openid.sig": "a-signature",
        "openid.signed": "signed,op_endpoint,claimed_id,identity,return_to",
        ...over,
    };
    for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
    return u;
}

function steamSays(body: string, status = 200) {
    const seen: { url: string; body: string } = { url: "", body: "" };
    vi.stubGlobal("fetch", async (url: string, init: RequestInit) => {
        seen.url = String(url);
        seen.body = String(init.body);
        return new Response(body, { status });
    });
    return seen;
}

describe("how Steam is configured", () => {
    it("is a provider the service knows", () => {
        expect(isProviderId("steam")).toBe(true);
        expect(PROVIDER_IDS).toContain("steam");
    });

    it("is OpenID 2.0: no token endpoint, no userinfo, no scope, no PKCE", () => {
        const p = PROVIDERS.steam;
        expect(p.flow).toBe("openid2");
        expect(p.tokenUrl).toBe("");   // OpenID 2.0 has no tokens to exchange
        expect(p.userUrl).toBe("");    // the assertion carries the id itself
        expect(p.scope).toBe("");      // the protocol has no such concept
        expect(p.pkce).toBe(false);
    });

    it("needs no credentials at all, so it works on a fresh deployment", () => {
        // The trap: requiring STEAM_CLIENT_ID would mean Steam silently never
        // appears, and no operator could work out why. There is nothing to set.
        expect(clientCredentials({}, "steam")).toEqual({ id: "", secret: "" });
    });

    it("CANNOT create an account, only link to one", () => {
        // timecreated exists only on public profiles -- absent precisely when
        // somebody has something to hide, so it cannot gate anything.
        expect(PROVIDERS.steam.canCreateAccount).toBe(false);
        expect(PROVIDERS.steam.accountCreatedAt({})).toBeNull();
    });

    it("collects no name, because none is offered", () => {
        expect(PROVIDERS.steam.suggestedName({})).toBeNull();
    });

    it("leaves the other providers' flows alone", () => {
        for (const id of ["google", "discord", "github"] as const) {
            expect(PROVIDERS[id].flow).toBe("code");
        }
        expect(PROVIDERS.itch.flow).toBe("implicit");
    });
});

describe("the SteamID64 out of the claimed identifier", () => {
    it("reads a well-formed one", () => {
        expect(PROVIDERS.steam.subjectOf({ "openid.claimed_id": CLAIMED })).toBe(STEAMID);
    });

    it("refuses anything that merely CONTAINS the Steam URL", () => {
        // The whole point of anchoring. claimed_id is attacker-written until
        // check_authentication passes, and an unanchored pattern would accept
        // a host that is not Steam at all.
        for (const bad of [
            `https://evil.example/steamcommunity.com/openid/id/${STEAMID}`,
            `https://steamcommunity.com.evil.example/openid/id/${STEAMID}`,
            `http://steamcommunity.com/openid/id/${STEAMID}`,          // not https
            `https://steamcommunity.com/openid/id/${STEAMID}/../9999`,
            "https://steamcommunity.com/openid/id/123",                 // too short
            "https://steamcommunity.com/openid/id/7656119800000000x",   // not digits
        ]) {
            expect(PROVIDERS.steam.subjectOf({ "openid.claimed_id": bad })).toBeNull();
        }
        expect(PROVIDERS.steam.subjectOf({})).toBeNull();
        expect(PROVIDERS.steam.subjectOf({ "openid.claimed_id": 5 })).toBeNull();
    });
});

describe("the request we send Steam", () => {
    it("is an OpenID 2.0 checkid_setup carrying the state in return_to", async () => {
        const built = await authorizeUrl(ENV, CLAIMS, "st");
        const u = new URL(String(built));
        expect(u.origin + u.pathname).toBe("https://steamcommunity.com/openid/login");
        expect(u.searchParams.get("openid.mode")).toBe("checkid_setup");
        expect(u.searchParams.get("openid.ns")).toBe("http://specs.openid.net/auth/2.0");

        // OpenID 2.0 has no state parameter; it goes in return_to, which Steam
        // signs. Losing this is how a callback becomes replayable.
        expect(u.searchParams.get("state")).toBeNull();
        expect(u.searchParams.get("openid.return_to")).toBe(RETURN_TO);
        expect(u.searchParams.get("openid.realm")).toBe("https://accounts.example.test");

        // No OAuth parameters leaked into a protocol that has none.
        for (const gone of ["client_id", "response_type", "scope", "code_challenge"]) {
            expect(u.searchParams.get(gone)).toBeNull();
        }
    });
});

describe("nothing is believed until Steam confirms it", () => {
    it("asks Steam via check_authentication and uses the answer", async () => {
        const seen = steamSays("ns:http://specs.openid.net/auth/2.0\nis_valid:true\n");
        const identity = await identityFromOpenId(ENV, "steam", callback());

        expect(seen.url).toBe("https://steamcommunity.com/openid/login");
        expect(seen.body).toContain("openid.mode=check_authentication");
        // Every signed field has to go back, or Steam cannot check the signature.
        expect(seen.body).toContain("openid.sig=");
        expect(seen.body).toContain("openid.signed=");
        expect(identity?.sub).toBe(STEAMID);
        expect(identity?.suggestedName).toBeNull();
        expect(identity?.createdAt).toBeNull();
    });

    it("refuses when Steam says the signature is not theirs", async () => {
        steamSays("ns:http://specs.openid.net/auth/2.0\nis_valid:false\n");
        expect(await identityFromOpenId(ENV, "steam", callback())).toBeNull();
    });

    it("does not read is_valid:false as a match for is_valid:true", async () => {
        // The bug this exists to prevent: a substring test on the body. The
        // response is line-oriented key:value, and "is_valid:false" contains
        // neither "is_valid:true" nor anything that should pass -- but a naive
        // `includes("is_valid:tru")`-style check, or matching without trimming
        // the line, is exactly the mistake that lets every forged callback in.
        steamSays("is_valid:false\nnote:is_valid:true is not what we said\n");
        expect(await identityFromOpenId(ENV, "steam", callback())).toBeNull();
    });

    it("refuses a forged callback without spending a request", async () => {
        // A return_to naming somebody else's site means the assertion was not
        // meant for us. Caught before the network call.
        let called = false;
        vi.stubGlobal("fetch", async () => { called = true; return new Response("is_valid:true"); });

        const forged = callback({ "openid.return_to": "https://evil.example/auth/callback/steam?state=st" });
        expect(await identityFromOpenId(ENV, "steam", forged)).toBeNull();
        expect(called).toBe(false);
    });

    it("ignores a cancelled sign-in rather than reporting a failure", async () => {
        let called = false;
        vi.stubGlobal("fetch", async () => { called = true; return new Response("is_valid:true"); });
        expect(await identityFromOpenId(ENV, "steam", callback({ "openid.mode": "cancel" }))).toBeNull();
        expect(called).toBe(false);
    });

    it("refuses when Steam itself errors", async () => {
        steamSays("", 500);
        expect(await identityFromOpenId(ENV, "steam", callback())).toBeNull();
    });

    it("refuses to run for a provider that is not OpenID 2.0", async () => {
        expect(await identityFromOpenId(ENV, "itch", callback())).toBeNull();
        expect(await identityFromOpenId(ENV, "google", callback())).toBeNull();
    });
});
