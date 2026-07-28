// End-to-end account lifecycle against a running `wrangler dev`.
//
//   npx wrangler dev --port 8787 --local &
//   node test/e2e.mjs
//
// or: npm run e2e
//
// WHY THIS EXISTS ALONGSIDE THE UNIT TESTS
//
// The unit tests exercise modules. This exercises the deployed shape: the
// router, the real KV binding, and -- crucially -- a signing key generated the
// way README.md tells an operator to generate one, by Node.
//
// That last part is not hypothetical. Node's exportKey stamps `alg: "Ed25519"`
// onto the JWK and workerd rejects exactly that, so every token silently failed
// to verify and the only symptom was "that sign-in expired" for every user.
// Nothing that tested modules in isolation could have caught it, because the
// tests generated their keys inside workerd.
//
// It forges a signup ticket with the dev key rather than driving a real OAuth
// round trip, which is the only part that needs a browser and a provider.

import { webcrypto as c } from "node:crypto";
import fs from "node:fs";

const vars = Object.fromEntries(fs.readFileSync(".dev.vars", "utf8").trim().split("\n")
  .map(l => { const i = l.indexOf("="); return [l.slice(0, i), l.slice(i + 1)]; }));
const B = "http://localhost:8787";
const b64u = (b) => Buffer.from(b).toString("base64url");

const key = await c.subtle.importKey("jwk", JSON.parse(vars.ED25519_PRIVATE_KEY),
                                     "Ed25519", false, ["sign"]);
async function signupTicket(subHash) {
  const now = Math.floor(Date.now() / 1000);
  const claims = { iss: vars.ISSUER, aud: "od-signup", provider: "discord",
                   subHash, iat: now, exp: now + 600 };
  const msg = `od1.${b64u(JSON.stringify(claims))}`;
  const sig = await c.subtle.sign("Ed25519", key, new TextEncoder().encode(msg));
  return `${msg}.${b64u(new Uint8Array(sig))}`;
}

const post = (p, body, token) => fetch(B + p, {
  method: "POST", headers: { "content-type": "application/json",
    ...(token ? { authorization: `Bearer ${token}` } : {}) },
  body: JSON.stringify(body),
}).then(async r => ({ status: r.status, json: await r.json().catch(() => ({})) }));
const get = (p, token) => fetch(B + p, {
  headers: token ? { authorization: `Bearer ${token}` } : {},
}).then(async r => ({ status: r.status, json: await r.json().catch(() => ({})) }));

let pass = 0, fail = 0;
const t = (name, ok, detail = "") => {
  if (ok) { console.log(`  ok    ${name}`); pass++; }
  else { console.log(`  FAIL  ${name} ${detail}`); fail++; }
};

// --- nickname preflight (no auth) ---
let r = await get("/nickname/check?n=Vlad");
t("a good nickname passes the public check", r.json.ok === true, JSON.stringify(r.json));
r = await get("/nickname/check?n=admin");
t("a reserved nickname is refused", r.json.reason === "reserved");
r = await get("/nickname/check?n=Adm1n");
t("leetspeak evasion is refused", r.json.reason === "reserved");
r = await get("/nickname/check?n=ab");
t("a short nickname is refused", r.json.reason === "too_short");

// --- signup ---
r = await post("/account/create", { signupTicket: await signupTicket("hash-user-one"), nickname: "Vlad" });
t("account creation succeeds", r.status === 200 && !!r.json.token, JSON.stringify(r.json));
const token = r.json.token;
t("and returns the account", r.json.account?.nickname === "Vlad");
t("with no badges yet", Array.isArray(r.json.account?.badges) && r.json.account.badges.length === 0);
t("and does not leak the subject hash", !JSON.stringify(r.json).includes("hash-user-one"));

// --- a second account cannot take the same name ---
r = await post("/account/create", { signupTicket: await signupTicket("hash-user-two"), nickname: "V_l_a_d" });
t("a normalized-duplicate nickname is refused", r.status === 409, JSON.stringify(r.json));

// --- me ---
r = await get("/account/me", token);
t("/account/me works with the token", r.status === 200 && r.json.account.nickname === "Vlad");
r = await get("/account/me");
t("and refuses without one", r.status === 401);

// --- nickname change + cooldown ---
r = await post("/account/nickname", { nickname: "Vladimir" }, token);
t("the first nickname change succeeds", r.status === 200 && r.json.account.nickname === "Vladimir", JSON.stringify(r.json));
r = await post("/account/nickname", { nickname: "Vladislav" }, token);
t("the second is refused by the cooldown", r.status === 429, JSON.stringify(r.json));
t("and the message says 7 days", /7 days/.test(r.json.message || ""), r.json.message);
r = await post("/account/nickname", { nickname: "hello!" }, token);
t("an invalid nickname is refused", r.status === 400 && r.json.error === "charset");

// --- badges (admin) ---
const accountId = (await get("/account/me", token)).json.account.id;
r = await fetch(B + "/admin/badge", { method: "POST",
  headers: { "content-type": "application/json", "x-od-admin": vars.ADMIN_SECRET },
  body: JSON.stringify({ accountId, badge: "developer", on: true }) })
  .then(async x => ({ status: x.status, json: await x.json() }));
t("an admin can grant a badge", r.status === 200 && r.json.account.badges.includes("developer"), JSON.stringify(r.json));
r = await fetch(B + "/admin/badge", { method: "POST",
  headers: { "content-type": "application/json", "x-od-admin": "wrong" },
  body: JSON.stringify({ accountId, badge: "developer", on: true }) });
t("a wrong admin secret gets 404, not 403", r.status === 404);

// --- export ---
r = await get("/account/export", token);
t("export returns every field", r.status === 200 && !!r.json.account.linkedIdentities[0].subjectHash);
t("and says plainly that no email was collected", /never collected your email/i.test(JSON.stringify(r.json)));

// --- delete, two steps ---
r = await post("/account/delete", {}, token);
t("delete step one explains rather than deleting", r.status === 200 && r.json.status === "confirm", JSON.stringify(r.json));
t("and names what it cannot reach", (r.json.cannotReach || []).join(" ").includes("separate data controllers"));
const confirmation = r.json.confirmation;
r = await get("/account/me", token);
t("the account still exists after step one", r.status === 200);

r = await post("/account/delete", { confirm: confirmation }, token);
t("delete step two removes it", r.status === 200 && r.json.status === "deleted", JSON.stringify(r.json));
r = await get("/account/me", token);
t("and the token no longer resolves", r.status === 401);

// --- tombstone ---
r = await post("/account/create", { signupTicket: await signupTicket("hash-user-three"), nickname: "Vladimir" });
t("the deleted nickname is held, not immediately reusable", r.status === 409 && r.json.error === "tombstoned", JSON.stringify(r.json));

console.log(`\n${pass + fail} checks, ${fail} failed`);
process.exit(fail === 0 ? 0 : 1);
