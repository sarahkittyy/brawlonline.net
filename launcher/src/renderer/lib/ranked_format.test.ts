import { describe, expect, it } from "vitest";

import type { MatchHistoryItem, MatchHistoryPlayer } from "@/services/backend/types";

import {
  formatMatchScore,
  formatPlace,
  formatPosition,
  formatRating,
  formatRatingChange,
  formatRecord,
  formatScore,
  matchNote,
  matchSides,
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

describe("room games", () => {
  const room = (mode: "1v1" | "ffa" | "teams", players: MatchHistoryPlayer[]) => ({
    mode: "room" as const,
    room: { code: "KFQB", mode },
    players,
  });
  // A place and slot (and team).
  const at = (uid: string, place: number | null, port: number, team: number | null = null) =>
    player(uid, place === 1 ? 1 : 0, { place, port, team });

  it("names the room's mode and notes the room", () => {
    expect([modeLabel("room", "1v1"), modeLabel("room", "ffa"), modeLabel("room", "teams")]).toEqual([
      "1v1",
      "FFA",
      "Teams",
    ]);
    expect(
      matchNote({ ranked: false, status: "ASSIGNED", endReason: null, players: [], room: room("ffa", []).room }),
    ).toBe("Room KFQB");
    expect([1, 2, 3, 4].map(formatPlace)).toEqual(["1st", "2nd", "3rd", "4th"]);
  });

  it("scores a free-for-all by my place, a team battle by my team's result, a 1v1 by games", () => {
    const ffa = room("ffa", [at("bob", 1, 1), at("ann", 3, 2), at("cy", 2, 3), at("dee", null, 4)]);
    expect(formatMatchScore(ffa, "ann")).toBe("3rd");
    expect(formatMatchScore(ffa, "bob")).toBe("1st");
    expect(formatMatchScore(ffa, "dee")).toBe("–");
    const teams = room("teams", [at("bob", 2, 1, 0), at("ann", 1, 2, 1), at("cy", 2, 3, 0), at("dee", 1, 4, 1)]);
    expect([formatMatchScore(teams, "ann"), formatMatchScore(teams, "cy")]).toEqual(["Won", "Lost"]);
    const draw = room("ffa", [at("bob", 1, 1), at("ann", 1, 2), at("cy", null, 3)]);
    expect(formatMatchScore(draw, "ann")).toBe("Draw");
    const unplaced = room("ffa", [at("bob", null, 1), at("ann", null, 2), at("cy", null, 3)]);
    expect(formatMatchScore(unplaced, "ann")).toBe("–");
    const oneVsOne = room("1v1", [at("bob", 1, 1), at("ann", 2, 3)]);
    expect(formatMatchScore(oneVsOne, "ann")).toBe("0 - 1");
  });

  it("puts my teammates on my side and the others in place order", () => {
    const ffa = room("ffa", [at("bob", 2, 1), at("ann", 3, 2), at("cy", null, 3), at("dee", 1, 4)]);
    const [mine, others] = matchSides(ffa, "ann");
    expect([mine.map((p) => p.uid), others.map((p) => p.uid)]).toEqual([["ann"], ["dee", "bob", "cy"]]);
    const teams = room("teams", [at("bob", 2, 1, 0), at("ann", 1, 2, 1), at("cy", 2, 3, 0), at("dee", 1, 4, 1)]);
    const [ours, theirs] = matchSides(teams, "ann");
    expect([ours.map((p) => p.uid), theirs.map((p) => p.uid)]).toEqual([
      ["ann", "dee"],
      ["bob", "cy"],
    ]);
    // Other modes: me against the rest, in the server's order.
    const [me, rest] = matchSides({ mode: "direct", room: null, players: [player("bob", 1), player("ann", 2)] }, "ann");
    expect([me.map((p) => p.uid), rest.map((p) => p.uid)]).toEqual([["ann"], ["bob"]]);
  });
});
