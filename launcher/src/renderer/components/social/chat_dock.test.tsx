import { describe, expect, it } from "vitest";

import { arrivedRoom, frontTab, inviteBlocked, useSocial, waitsForRoom } from "@/lib/social/use_social";
import { volumeOf } from "@/lib/social/volume";

import { chatSegments, deleteWordBefore } from "./chat_dock";

describe("chat box", () => {
  it("Ctrl+W deletes the word before the caret", () => {
    expect(deleteWordBefore("hello there bob", 15)).toEqual({ text: "hello there ", caret: 12 });
    expect(deleteWordBefore("hello there ", 12)).toEqual({ text: "hello ", caret: 6 });
    expect(deleteWordBefore("hello there", 5)).toEqual({ text: " there", caret: 0 });
    expect(deleteWordBefore("", 0)).toEqual({ text: "", caret: 0 });
  });

  it("links in a message: http(s), www. and discord.gg, without the sentence's punctuation", () => {
    expect(chatSegments("add me")).toEqual([{ text: "add me" }]);
    expect(chatSegments("")).toEqual([{ text: "" }]);
    expect(chatSegments("join discord.gg/abc123.")).toEqual([
      { text: "join " },
      { text: "discord.gg/abc123", url: "https://discord.gg/abc123" },
      { text: "." },
    ]);
    expect(chatSegments("(see https://x.com/a_b?c=1) ok")).toEqual([
      { text: "(see " },
      { text: "https://x.com/a_b?c=1", url: "https://x.com/a_b?c=1" },
      { text: ") ok" },
    ]);
    expect(chatSegments("https://en.wikipedia.org/wiki/Brawl_(game)!")[0]).toEqual({
      text: "https://en.wikipedia.org/wiki/Brawl_(game)",
      url: "https://en.wikipedia.org/wiki/Brawl_(game)",
    });
    expect(chatSegments("www.twitch.tv/me")).toEqual([{ text: "www.twitch.tv/me", url: "https://www.twitch.tv/me" }]);
    // Not links: inside a word, no dotted host, other schemes.
    expect(chatSegments("awww.cute")).toEqual([{ text: "awww.cute" }]);
    expect(chatSegments("http://localhost")).toEqual([{ text: "http://localhost" }]);
    expect(chatSegments("javascript:alert(1) file:///c:/x")).toEqual([{ text: "javascript:alert(1) file:///c:/x" }]);
  });

  it("invites are greyed out only in a full room", () => {
    const room = { code: "KFQB", players: 2, openSlots: 2, mode: "1v1" as const, status: "waiting" as const };
    expect(inviteBlocked(null)).toBeUndefined();
    expect(inviteBlocked({ ...room, joinable: true })).toBeUndefined();
    expect(inviteBlocked({ ...room, joinable: false })).toBe("full");
  });

  it("notification volume: 0-100 % from the setting, 15 % without one", () => {
    expect(volumeOf(15)).toBe(0.15);
    expect(volumeOf(100)).toBe(1);
    expect(volumeOf(250)).toBe(1);
    expect(volumeOf(-3)).toBe(0);
    expect(volumeOf(undefined)).toBe(0.15);
    expect(volumeOf("loud")).toBe(0.15);
  });

  it("invites wait for a room the player asked for until a new room shows up", () => {
    const room = (code: string) => ({
      code,
      players: 1,
      openSlots: 2,
      mode: "1v1" as const,
      status: "waiting" as const,
      joinable: true,
    });
    const fresh = { since: 0, fromCode: null, invites: [] };
    expect(waitsForRoom(null, null)).toBe(false);
    expect(waitsForRoom(fresh, null)).toBe(true);
    expect(arrivedRoom(fresh, room("KFQB"))?.code).toBe("KFQB");
    expect(waitsForRoom(fresh, room("KFQB"))).toBe(false);
    // Created from inside a room: the old room doesn't count, the new one does.
    const fromRoom = { since: 0, fromCode: "BCDF", invites: [] };
    expect(waitsForRoom(fromRoom, room("BCDF"))).toBe(true);
    expect(waitsForRoom(fromRoom, null)).toBe(true);
    expect(arrivedRoom(fromRoom, room("KFQB"))?.code).toBe("KFQB");
  });

  it("the window opened or clicked last is drawn over the others", () => {
    const social = useSocial.getState();
    const front = () => frontTab(useSocial.getState().tabs);
    const open = (uid: string) => useSocial.getState().tabs.find((t) => t.uid === uid)?.open;
    useSocial.setState({ tabs: [] });
    social.openTab("a", true);
    social.openTab("b", true);
    expect(front()).toBe("b");
    // A message opens a tab without its window: nothing moves.
    social.openTab("c", false);
    expect(front()).toBe("b");
    // A covered window's tab brings it up instead of minimizing it; the front one's minimizes.
    social.clickTab("a");
    expect([front(), open("a")]).toEqual(["a", true]);
    social.clickTab("a");
    expect([front(), open("a")]).toEqual(["b", false]);
    // Clicking inside a window, or Message on an already-open chat, raises it.
    social.clickTab("c");
    expect(front()).toBe("c");
    social.raiseTab("b");
    expect(front()).toBe("b");
    social.openTab("c", true);
    expect(front()).toBe("c");
    social.closeTab("c");
    expect(front()).toBe("b");
    useSocial.setState({ tabs: [] });
  });
});
