import { describe, expect, it } from "vitest";

import type { MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import {
  formatPosition,
  formatRating,
  formatRatingChange,
  formatRecord,
  formatScore,
  matchNote,
  modeLabel,
  orderPlayers,
  playerName,
  RATING_DOWN_COLOR,
  RATING_UP_COLOR,
  ratingChangeColor,
} from "./ranked_format";

const player = (uid: string, wins: number, overrides: Partial<MatchHistoryPlayer> = {}): MatchHistoryPlayer => ({
  uid,
  displayName: uid,
  connectCode: `${uid.toUpperCase()}#1`,
  wins,
  ratingBefore: null,
  ratingAfter: null,
  ratingChange: null,
  ...overrides,
});

const set = (status: string, endReason: string | null = null): MatchHistoryItem => ({
  matchId: "m",
  mode: "ranked",
  createdAt: "2026-10-08T20:00:00Z",
  status,
  ranked: true,
  players: [player("ann", 0), player("bob", 1)],
  winner: null,
  endReason,
});

describe("ranked formatting", () => {
  it("shows ratings with one decimal and signed changes", () => {
    expect(formatRating(1523.44)).toBe("1523.4");
    expect(formatRating(1400)).toBe("1400.0");
    expect(formatRatingChange(16)).toBe("+16.0");
    expect(formatRatingChange(-12.345)).toBe("-12.3");
    expect(formatRatingChange(0)).toBe("±0.0");
    // A change that rounds to zero gets no sign and no color.
    expect(formatRatingChange(-0.04)).toBe("±0.0");
    expect(ratingChangeColor(-0.04)).toBeUndefined();
    expect(ratingChangeColor(3)).toBe(RATING_UP_COLOR);
    expect(ratingChangeColor(-3)).toBe(RATING_DOWN_COLOR);
  });

  it("formats records, positions and scores", () => {
    expect(formatRecord(12, 3)).toBe("12-3");
    expect(formatPosition(23, 137)).toBe("#23 of 137");
    expect(formatPosition(1, 0)).toBe("#1");
    expect(formatPosition(null, 137)).toBeNull();
    expect(formatPosition(undefined, undefined)).toBeNull();
    expect(formatScore([player("a", 2), player("b", 1)])).toBe("2 - 1");
  });

  it("names modes and players", () => {
    expect([modeLabel("ranked"), modeLabel("unranked"), modeLabel("direct")]).toEqual(["Ranked", "Unranked", "Direct"]);
    expect(playerName({ displayName: "" })).toBe("Unknown player");
    expect(playerName({ displayName: "ann" })).toBe("ann");
  });

  it("puts the logged-in player first", () => {
    const ps = [player("bob", 2), player("ann", 1)];
    expect(orderPlayers(ps, "ann").map((p) => p.uid)).toEqual(["ann", "bob"]);
    expect(orderPlayers(ps, "zed").map((p) => p.uid)).toEqual(["bob", "ann"]);
    expect(orderPlayers(ps, undefined).map((p) => p.uid)).toEqual(["bob", "ann"]);
  });

  it("notes sets that did not simply finish", () => {
    expect(matchNote(set("COMPLETE"))).toBeNull();
    expect(matchNote({ ...set("ASSIGNED"), ranked: false })).toBeNull();
    expect(matchNote(set("ASSIGNED"))).toBe("In progress");
    expect(matchNote(set("ABANDONED", "abandoned by bob"))).toBe("Abandoned by bob");
    expect(matchNote(set("ABANDONED", "abandoned by someone-else"))).toBe("Abandoned");
    expect(matchNote(set("TERMINATED"))).toMatch(/^Void: the connection broke/);
    expect(matchNote(set("ERROR"))).toMatch(/^Void: the reports disagree/);
    expect(matchNote(set("ORPHANED"))).toMatch(/^Void: no result/);
  });
});
