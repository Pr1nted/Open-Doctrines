// Launcher statistics: only the fixed vocabulary crosses to Google, and no
// free text can, whatever a client sends.
import { describe, expect, it } from "vitest";
import { cleanEvents } from "../src/analytics/ga.js";

describe("the analytics relay forwards a closed vocabulary", () => {
    it("keeps a known event and its known parameters", () => {
        const out = cleanEvents([{ name: "game_launch", params: { game_version: "1.2.2a", platform: "macos-arm64", source: "play" } }]);
        expect(out).toEqual([{ name: "game_launch", params: { game_version: "1.2.2a", platform: "macos-arm64", source: "play" } }]);
    });
    it("drops unknown events", () => {
        expect(cleanEvents([{ name: "world_name", params: {} }])).toEqual([]);
    });
    it("drops unknown parameters and anything that is not a short token", () => {
        const out = cleanEvents([{ name: "tab_view", params: { tab: "my secret world name", nick: "Vlad" } }]);
        expect(out).toEqual([{ name: "tab_view", params: {} }]);
    });
    it("caps a batch", () => {
        expect(cleanEvents(Array.from({ length: 50 }, () => ({ name: "launcher_open" }))).length).toBe(20);
    });
});
