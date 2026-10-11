import { describe, expect, it } from "vitest";

import { inviteBlocked } from "@/lib/social/use_social";

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
});
