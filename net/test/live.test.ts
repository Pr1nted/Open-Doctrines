// Reading "is this channel live" out of three different answers.
//
// Wrong in the FALSE direction is invisible -- nobody is ever marked live.
// Wrong in the TRUE direction is worse -- everybody is, permanently. Both are
// one missing field away, so every shape each platform actually sends is
// pinned here.

import { describe, expect, it } from "vitest";
import { readKick, readTwitch, readYouTube, readFor, safeChannel } from "../src/live/platforms.js";

describe("Twitch", () => {
    it("reads a live stream", () => {
        const r = readTwitch({ data: [{ type: "live", title: "Tournament!", viewer_count: 412 }] });
        expect(r.live).toBe(true);
        expect(r.title).toBe("Tournament!");
        expect(r.viewers).toBe(412);
    });
    it("reads an EMPTY list as offline, which is how Twitch says it", () => {
        expect(readTwitch({ data: [] }).live).toBe(false);
    });
    it("refuses a row that is not a live type", () => {
        expect(readTwitch({ data: [{ type: "rerun" }] }).live).toBe(false);
    });
    it("survives rubbish", () => {
        expect(readTwitch(null).live).toBe(false);
        expect(readTwitch({}).live).toBe(false);
        expect(readTwitch("nope").live).toBe(false);
        expect(readTwitch({ data: "nope" }).live).toBe(false);
    });
});

describe("YouTube", () => {
    it("reads a live broadcast", () => {
        const r = readYouTube({ items: [{ snippet: { liveBroadcastContent: "live",
                                                     title: "OD tournament" } }] });
        expect(r.live).toBe(true);
        expect(r.title).toBe("OD tournament");
    });
    it("does NOT read an upcoming stream as live", () => {
        // The commonest way this goes wrong in the true direction: a scheduled
        // stream answers with items, and only the snippet says it has not
        // started.
        expect(readYouTube({ items: [{ snippet: { liveBroadcastContent: "upcoming" } }] }).live)
            .toBe(false);
        expect(readYouTube({ items: [{ snippet: { liveBroadcastContent: "none" } }] }).live)
            .toBe(false);
    });
    it("reads no items as offline", () => {
        expect(readYouTube({ items: [] }).live).toBe(false);
    });
    it("survives rubbish", () => {
        expect(readYouTube(null).live).toBe(false);
        expect(readYouTube({ items: [{}] }).live).toBe(false);
    });
});

describe("Kick", () => {
    it("reads a live channel", () => {
        const r = readKick({ data: [{ stream_title: "OD!",
                                      stream: { is_live: true, viewer_count: 88 } }] });
        expect(r.live).toBe(true);
        expect(r.viewers).toBe(88);
    });
    it("reads a null stream as offline", () => {
        expect(readKick({ data: [{ stream: null }] }).live).toBe(false);
    });
    it("reads a stream that exists but has ended as offline", () => {
        // A channel that just went off air still carries the object.
        expect(readKick({ data: [{ stream: { is_live: false } }] }).live).toBe(false);
    });
    it("survives rubbish", () => {
        expect(readKick(null).live).toBe(false);
        expect(readKick({ data: [] }).live).toBe(false);
    });
});

describe("dispatch and channel names", () => {
    it("sends each platform to its own reader", () => {
        expect(readFor("twitch", { data: [{ type: "live" }] }).live).toBe(true);
        expect(readFor("kick", { data: [{ stream: { is_live: true } }] }).live).toBe(true);
        expect(readFor("youtube", { items: [] }).live).toBe(false);
    });
    it("refuses a channel name that is not a plain handle", () => {
        // These go into a URL. Refused rather than escaped: there is no
        // legitimate channel with a slash in it, and a refusal is one fewer
        // place to have got the escaping right.
        expect(safeChannel("streamer")).toBe("streamer");
        expect(safeChannel("#streamer")).toBe("streamer");
        expect(safeChannel("UC_x-123")).toBe("UC_x-123");
        expect(safeChannel("a/../b")).toBeNull();
        expect(safeChannel("a?b=1")).toBeNull();
        expect(safeChannel("has space")).toBeNull();
        expect(safeChannel("")).toBeNull();
        expect(safeChannel("x".repeat(65))).toBeNull();
    });
});
