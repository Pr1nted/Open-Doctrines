import { beforeAll, describe, expect, it } from "vitest";
import type { Env } from "../src/env.js";
import {
    canonicalize, checkAgainstBlocklist, checkNickname, checkShape,
    normalize, parseBlocklist,
} from "../src/accounts/nickname.js";
import { setupEnv } from "./helpers.js";

let env: Env;
beforeAll(async () => { env = await setupEnv(); });

describe("canonicalize", () => {
    it("folds fullwidth and accented forms onto plain ASCII", () => {
        expect(canonicalize("Ｖｌａｄ")).toBe("Vlad");
        expect(canonicalize("Ádmin")).toBe("Admin");
    });

    it("folds Cyrillic and Greek lookalikes, preserving case", () => {
        // Cyrillic А, і — renders identically to "Admin" in any game font.
        expect(canonicalize("Аdmіn")).toBe("Admin");
        expect(canonicalize("ΡΑYΡΑL")).toBe("PAYPAL");
    });

    it("folds mathematical alphanumerics, a common filter dodge", () => {
        // NFKC maps these onto plain ASCII, so styled text is not a way past
        // the reserved-word list.
        expect(canonicalize("𝓪𝓭𝓶𝓲𝓷")).toBe("admin");
        expect(canonicalize("𝐀𝐝𝐦𝐢𝐧")).toBe("Admin");
    });

    it("leaves ordinary names alone", () => {
        expect(canonicalize("Vlad_99")).toBe("Vlad_99");
    });
});

describe("normalize", () => {
    it("undoes separators, leetspeak and repeats", () => {
        expect(normalize("A_d-m.i n")).toBe("admin");
        expect(normalize("Adm1n")).toBe("admin");
        expect(normalize("aaadmiiin")).toBe("admin");
        expect(normalize("@DM1N")).toBe("admin");
    });
});

describe("checkShape", () => {
    const accepted = ["Vlad", "vlad_99", "a-b-c", "Some Name", "x1y", "A.B.C"];
    for (const name of accepted) {
        it(`accepts ${JSON.stringify(name)}`, () => {
            expect(checkShape(name).ok).toBe(true);
        });
    }

    const rejected: Array<[string, string]> = [
        ["ab", "too_short"],
        ["a".repeat(25), "too_long"],
        ["hello!", "charset"],
        ["hello🙂", "charset"],
        ["_vlad", "separator"],
        ["vlad_", "separator"],
        ["vlad__x", "separator"],
        ["vlad -x", "separator"],
        ["12345", "no_letter"],
    ];
    for (const [name, reason] of rejected) {
        it(`rejects ${JSON.stringify(name)} as ${reason}`, () => {
            const result = checkShape(name);
            expect(result.ok).toBe(false);
            expect(result.reason).toBe(reason);
        });
    }

    it("counts length in code points, not UTF-16 units", () => {
        // 13 Gothic letters: 13 code points but 26 UTF-16 units. Counting
        // units would report "too_long" for a name that is within the limit;
        // the real problem with it is the charset, and that is what should be
        // reported.
        expect(checkShape("\u{10330}".repeat(13)).reason).toBe("charset");
    });
});

describe("reserved words", () => {
    const list = parseBlocklist("");

    for (const attempt of ["admin", "Adm1n", "A_D_M_I_N", "xXadminXx", "aaadmin", "Аdmin"]) {
        it(`refuses ${JSON.stringify(attempt)}`, () => {
            const result = checkAgainstBlocklist(checkShape(attempt), list);
            expect(result.ok).toBe(false);
            expect(result.reason).toBe("reserved");
        });
    }

    it("refuses badge names, which are claims about who you are", () => {
        expect(checkAgainstBlocklist(checkShape("developer"), list).reason).toBe("reserved");
        expect(checkAgainstBlocklist(checkShape("PlayTester"), list).reason).toBe("reserved");
    });
});

describe("blocklist", () => {
    // A deliberately mild stand-in: the real list is uploaded at deploy time
    // and is not in this repository.
    const list = parseBlocklist([
        "# comment",
        "badword",
        "!scunthorpe",         // exception for a legitimate name
    ].join("\n"));

    it("matches through leetspeak and separators", () => {
        expect(checkAgainstBlocklist(checkShape("b4dw0rd"), list).reason).toBe("blocked");
        expect(checkAgainstBlocklist(checkShape("b_a_d_w_o_r_d"), list).reason).toBe("blocked");
        expect(checkAgainstBlocklist(checkShape("xxbadwordxx"), list).reason).toBe("blocked");
    });

    it("honours exceptions, so a real name is not caught by a substring", () => {
        const list2 = parseBlocklist(["cunt", "!scunthorpe"].join("\n"));
        expect(checkAgainstBlocklist(checkShape("Scunthorpe"), list2).ok).toBe(true);
        expect(checkAgainstBlocklist(checkShape("Cunt"), list2).ok).toBe(false);
    });

    it("ignores comments and blank lines", () => {
        expect(parseBlocklist("# nothing\n\n   \n").terms).toEqual([]);
    });
});

describe("checkNickname", () => {
    it("falls back to shape and reserved words when no list is deployed", async () => {
        expect((await checkNickname(env, "Vlad")).ok).toBe(true);
        expect((await checkNickname(env, "admin")).reason).toBe("reserved");
        expect((await checkNickname(env, "ab")).reason).toBe("too_short");
    });
});
