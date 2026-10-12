import { render } from "@testing-library/react";
import React from "react";
import { describe, expect, it } from "vitest";

import { RATING_DOWN_COLOR, RATING_UP_COLOR } from "@/lib/ranked_format";
import { LeaderboardRow } from "@/pages/leaderboard/leaderboard_page";
import type { MatchHistoryItem } from "@/services/backend/types";

import { MatchRow } from "./profile_page";

const inTable = (row: React.ReactNode) => (
  <table>
    <tbody>{row}</tbody>
  </table>
);

const rankedSet: MatchHistoryItem = {
  matchId: "mode.ranked-1",
  mode: "ranked",
  createdAt: "2026-10-08T20:00:00Z",
  status: "COMPLETE",
  ranked: true,
  // mm's order: the opponent first.
  players: [
    {
      uid: "opp",
      displayName: "bob",
      connectCode: "BOB#2",
      wins: 1,
      ratingBefore: 1500,
      ratingAfter: 1484,
      ratingChange: -16,
    },
    {
      uid: "me",
      displayName: "ann",
      connectCode: "ANN#1",
      wins: 2,
      ratingBefore: 1450.25,
      ratingAfter: 1466.25,
      ratingChange: 16,
    },
  ],
  winner: "me",
  endReason: null,
};

describe("MatchRow", () => {
  it("shows the mode, both players, the score from my side and each rating change in color", () => {
    const { container } = render(inTable(<MatchRow item={rankedSet} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("Ranked");
    expect(cells[2]).toBe("annANN#11450.3 +16.0");
    expect(cells[3]).toBe("2 - 1");
    expect(cells[4]).toBe("bobBOB#21500.0 -16.0");
    const colored = Array.from(container.querySelectorAll("span")).filter((s) => /^[+-]/.test(s.textContent ?? ""));
    expect(colored.map((s) => s.style.color)).toEqual([hexToRgb(RATING_UP_COLOR), hexToRgb(RATING_DOWN_COLOR)]);
  });

  it("notes an abandoned set and shows no rating for a player it did not change", () => {
    const abandoned: MatchHistoryItem = {
      ...rankedSet,
      status: "ABANDONED",
      endReason: "abandoned by opp",
      winner: null,
      players: [
        { ...rankedSet.players[0], wins: 0 },
        { ...rankedSet.players[1], wins: 0, ratingBefore: null, ratingAfter: null, ratingChange: null },
      ],
    };
    const { container } = render(inTable(<MatchRow item={abandoned} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("RankedAbandoned by bob");
    expect(cells[2]).toBe("annANN#1");
    expect(cells[3]).toBe("0 - 0");
  });

  it("shows Unranked and Direct matches without ratings", () => {
    const direct: MatchHistoryItem = {
      ...rankedSet,
      mode: "direct",
      ranked: false,
      status: "ASSIGNED",
      winner: null,
      players: rankedSet.players.map((p) => ({ ...p, ratingBefore: null, ratingAfter: null, ratingChange: null })),
    };
    const { container } = render(inTable(<MatchRow item={direct} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("Direct");
    expect(cells[2]).toBe("annANN#1");
    expect(cells[4]).toBe("bobBOB#2");
  });

  const roomPlayer = (uid: string, name: string, place: number | null, port: number, team: number | null = null) => ({
    uid,
    displayName: name,
    connectCode: `${name.toUpperCase()}#${port}`,
    wins: place === 1 ? 1 : 0,
    ratingBefore: null,
    ratingAfter: null,
    ratingChange: null,
    place,
    port,
    team,
  });
  const roomGame = (mode: "1v1" | "ffa" | "teams", players: MatchHistoryItem["players"]): MatchHistoryItem => ({
    matchId: "mode.room-KFQB-1",
    mode: "room",
    createdAt: "2026-10-08T20:00:00Z",
    status: "ASSIGNED",
    ranked: false,
    players,
    winner: null,
    endReason: null,
    room: { code: "KFQB", mode },
  });

  it("shows a free-for-all with my place and everyone else's, best first", () => {
    const ffa = roomGame("ffa", [
      roomPlayer("b", "bob", 3, 1),
      roomPlayer("me", "ann", 2, 2),
      roomPlayer("c", "cy", 1, 3),
      roomPlayer("d", "dee", null, 4),
    ]);
    const { container } = render(inTable(<MatchRow item={ffa} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("FFARoom KFQB");
    expect(cells[2]).toBe("annANN#2");
    expect(cells[3]).toBe("2nd");
    expect(cells[4]).toBe("cyCY#31stbobBOB#13rddeeDEE#4");
  });

  it("shows a team battle with my teammate on my side, team colours and the result", () => {
    const teams = roomGame("teams", [
      roomPlayer("b", "bob", 2, 1, 0),
      roomPlayer("me", "ann", 1, 2, 1),
      roomPlayer("c", "cy", 2, 3, 0),
      roomPlayer("d", "dee", 1, 4, 1),
    ]);
    const { container } = render(inTable(<MatchRow item={teams} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("TeamsRoom KFQB");
    expect(cells[2]).toBe("annANN#2deeDEE#4");
    expect(cells[3]).toBe("Won");
    expect(cells[4]).toBe("bobBOB#1cyCY#3");
    const dots = Array.from(container.querySelectorAll("span[aria-label]")).map((s) => s.getAttribute("aria-label"));
    expect(dots).toEqual(["Blue team", "Blue team", "Red team", "Red team"]);
  });

  it("shows a room 1v1 by games won", () => {
    const duel = roomGame("1v1", [roomPlayer("b", "bob", 1, 1), roomPlayer("me", "ann", 2, 3)]);
    const { container } = render(inTable(<MatchRow item={duel} myUid="me" />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells[0]).toBe("1v1Room KFQB");
    expect(cells[3]).toBe("0 - 1");
    expect(cells[4]).toBe("bobBOB#1");
  });

  it("links a room game's published ruleset to its page", () => {
    const duel = roomGame("1v1", [roomPlayer("b", "bob", 1, 1), roomPlayer("me", "ann", 2, 3)]);
    const link = "https://brawlonline.net/r/BCDFGH";
    duel.room = { code: "KFQB", mode: "1v1", ruleset: { code: "BCDFGH", name: "NorCal Weekly", version: 3, link } };
    const { container } = render(inTable(<MatchRow item={duel} myUid="me" />));
    const a = container.querySelector("td a");
    expect(a?.textContent).toBe("NorCal Weekly v3");
    expect(a?.getAttribute("href")).toBe(link);
  });
});

describe("LeaderboardRow", () => {
  it("shows position, name, code, rating with one decimal and W-L, and marks my row", () => {
    const entry = {
      position: 7,
      uid: "me",
      displayName: "ann",
      connectCode: "ANN#1",
      rating: 1612.345,
      setsPlayed: 15,
      wins: 10,
      losses: 5,
    };
    const { container } = render(inTable(<LeaderboardRow entry={entry} isMe={true} />));
    const cells = Array.from(container.querySelectorAll("td")).map((td) => td.textContent);
    expect(cells).toEqual(["7", "ann", "ANN#1", "1612.3", "10-5"]);
    expect(container.querySelector("tr")!.getAttribute("data-me")).toBe("true");
    const { container: other } = render(inTable(<LeaderboardRow entry={{ ...entry, uid: "x" }} isMe={false} />));
    expect(other.querySelector("tr")!.getAttribute("data-me")).toBeNull();
  });
});

function hexToRgb(hex: string): string {
  const n = parseInt(hex.slice(1), 16);
  return `rgb(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255})`;
}
