import { describe, expect, it } from "vitest";

import { inviteBlocked } from "@/lib/social/use_social";
import { volumeOf } from "@/lib/social/volume";

import { deleteWordBefore } from "./chat_dock";

describe("chat box", () => {
  it("Ctrl+W deletes the word before the caret", () => {
    expect(deleteWordBefore("hello there bob", 15)).toEqual({ text: "hello there ", caret: 12 });
    expect(deleteWordBefore("hello there ", 12)).toEqual({ text: "hello ", caret: 6 });
    expect(deleteWordBefore("hello there", 5)).toEqual({ text: " there", caret: 0 });
    expect(deleteWordBefore("", 0)).toEqual({ text: "", caret: 0 });
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
});
