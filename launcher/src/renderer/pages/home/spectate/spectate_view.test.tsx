import type { LiveGameEntry } from "@spectate/types";
import { fireEvent, render } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { SpectateMessages as Messages } from "./spectate.messages";
import type { SpectateListState } from "./spectate_view";
import { gameClock, setScore, SpectateView } from "./spectate_view";

const ME = "me";
const NOW = Date.parse("2026-10-14T02:10:00Z");

function game(id: string, startedAt: string, patch: Partial<LiveGameEntry> = {}): LiveGameEntry {
  return {
    id,
    mode: "ranked",
    room: null,
    isTeams: false,
    startedAt,
    spectators: 0,
    game: { number: 1, place: 1, startedAt, stageId: 0x02, state: "playing" },
    players: [
      { uid: `${id}-a`, displayName: "ann", connectCode: "ANN#1", characterId: 2, costume: 0, teamId: null, wins: 0 },
      { uid: `${id}-b`, displayName: "bob", connectCode: "BOB#2", characterId: 9, costume: 0, teamId: null, wins: 0 },
    ],
    ...patch,
  };
}

const games: LiveGameEntry[] = [
  game("aaaaaaaaaaaa", "2026-10-14T02:00:00Z", { spectators: 12 }),
  game("bbbbbbbbbbbb", "2026-10-14T02:06:00Z", {
    mode: "direct",
    game: { number: 2, place: 2, startedAt: "2026-10-14T02:08:55Z", stageId: 0x02, state: "playing" },
    players: [
      { uid: ME, displayName: "me", connectCode: "ME#1", characterId: 1, costume: 0, teamId: null, wins: 1 },
      { uid: "x", displayName: "cy", connectCode: "CY#3", characterId: 4, costume: 0, teamId: null, wins: 0 },
    ],
  }),
  game("cccccccccccc", "2026-10-14T02:04:00Z", {
    mode: "unranked",
    game: { number: 1, place: 1, startedAt: "2026-10-14T02:04:00Z", stageId: 0x02, state: "between" },
  }),
];

const view = (state: SpectateListState, props: Partial<Parameters<typeof SpectateView>[0]> = {}) =>
  render(
    <SpectateView
      state={state}
      uid={ME}
      autoPlay={false}
      onAutoPlayChange={vi.fn()}
      onWatch={vi.fn()}
      onRetry={vi.fn()}
      onLogIn={vi.fn()}
      now={NOW}
      {...props}
    />,
  );

describe("SpectateView", () => {
  it("lists the live games newest first, with who plays, the game, its time and how many watch", () => {
    const { container } = view({ kind: "ready", games });
    const rows = Array.from(container.querySelectorAll("button[data-stream]"));
    expect(rows.map((r) => r.getAttribute("data-stream"))).toEqual(["bbbbbbbbbbbb", "aaaaaaaaaaaa", "cccccccccccc"]);
    const text = (id: string) => container.querySelector(`[data-stream="${id}"]`)!.textContent!;
    expect(text("aaaaaaaaaaaa")).toContain("ann");
    expect(text("aaaaaaaaaaaa")).toContain("bob");
    expect(text("aaaaaaaaaaaa")).toContain("Ranked");
    expect(text("aaaaaaaaaaaa")).toContain("10:00");
    expect(text("aaaaaaaaaaaa")).toContain("Final Destination");
    const eye = container.querySelector('[data-stream="aaaaaaaaaaaa"] [data-testid="spectators"]')!;
    expect(eye.textContent).toBe("12");
    expect(text("bbbbbbbbbbbb")).toContain("Game 2 · 1-0");
    expect(text("bbbbbbbbbbbb")).toContain("1:05");
    expect(text("cccccccccccc")).toContain(Messages.betweenGames());
  });

  it("sorts by the game clock, not by when the set began", () => {
    // Production 2026-10-15: rows by set start showed clocks of 1:26, 2:27, 0:53, 0:37 ... 7:19.
    const later = game("dddddddddddd", "2026-10-14T01:50:00Z", {
      game: { number: 3, place: 3, startedAt: "2026-10-14T02:09:30Z", stageId: 0x02, state: "playing" },
    });
    const over = game("eeeeeeeeeeee", "2026-10-14T02:09:50Z", {
      game: { number: 1, place: 1, startedAt: "2026-10-14T02:09:50Z", stageId: 0x02, state: "over" },
    });
    const { container } = view({ kind: "ready", games: [over, ...games, later] });
    const rows = Array.from(container.querySelectorAll("button[data-stream]"));
    expect(rows.map((r) => r.getAttribute("data-stream"))).toEqual([
      "dddddddddddd",
      "bbbbbbbbbbbb",
      "aaaaaaaaaaaa",
      "cccccccccccc",
      "eeeeeeeeeeee",
    ]);
  });

  it("watches a game on a click, but not the player's own", () => {
    const onWatch = vi.fn();
    const { container } = view({ kind: "ready", games }, { onWatch });
    for (const id of ["aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc"]) {
      fireEvent.click(container.querySelector(`[data-stream="${id}"]`)!);
    }
    expect(onWatch.mock.calls.map((c) => c[0].id)).toEqual(["aaaaaaaaaaaa", "cccccccccccc"]);
    expect(container.querySelector('[data-stream="bbbbbbbbbbbb"]')!.textContent).toContain(Messages.yourGame());
  });

  it("marks the game being watched, and waits for one watch at a time", () => {
    const onWatch = vi.fn();
    const { container } = view(
      { kind: "ready", games },
      {
        onWatch,
        watching: { status: "watching", stream: "aaaaaaaaaaaa", waitingForGame: false },
        startingId: "cccccccccccc",
      },
    );
    expect(container.querySelector('[data-stream="aaaaaaaaaaaa"]')!.textContent).toContain(Messages.watching());
    fireEvent.click(container.querySelector('[data-stream="aaaaaaaaaaaa"]')!);
    fireEvent.click(container.querySelector('[data-stream="cccccccccccc"]')!);
    expect(onWatch).not.toHaveBeenCalled();
  });

  it("turns Auto play on and off", () => {
    const onAutoPlayChange = vi.fn();
    const { container } = view({ kind: "ready", games }, { onAutoPlayChange });
    fireEvent.click(container.querySelector(`input[aria-label="${Messages.autoPlay()}"]`)!);
    expect(onAutoPlayChange).toHaveBeenCalledWith(true);
  });

  it("puts ended streams at the bottom with a red ENDED, still watchable", () => {
    const onWatch = vi.fn();
    const ended = game("dddddddddddd", "2026-10-14T02:09:00Z", {
      game: { number: 2, place: 2, startedAt: "2026-10-14T02:09:30Z", stageId: 0x02, state: "over" },
    });
    const { container } = view({ kind: "ready", games: [ended, ...games] }, { onWatch });
    const rows = Array.from(container.querySelectorAll("button[data-stream]"));
    expect(rows.map((r) => r.getAttribute("data-stream"))).toEqual([
      "bbbbbbbbbbbb",
      "aaaaaaaaaaaa",
      "cccccccccccc",
      "dddddddddddd",
    ]);
    const time = container.querySelector('[data-stream="dddddddddddd"] [data-testid="game-time"]')!;
    expect(time.textContent).toBe(Messages.ended());
    expect(time.className).toMatch(/ended/);
    fireEvent.click(container.querySelector('[data-stream="dddddddddddd"]')!);
    expect(onWatch.mock.calls.map((c) => c[0].id)).toEqual(["dddddddddddd"]);
  });

  it("shows the empty, logged-out and unreachable states", () => {
    expect(view({ kind: "ready", games: [] }).getByText(Messages.noGames())).toBeTruthy();
    expect(view({ kind: "logged-out" }).getByText(Messages.logIn())).toBeTruthy();
    expect(view({ kind: "unreachable" }).getByText(Messages.retry())).toBeTruthy();
  });

  it("counts the game's time and the set's score", () => {
    expect(gameClock("2026-10-14T02:05:55Z", NOW)).toBe("4:05");
    expect(gameClock("2026-10-14T02:15:00Z", NOW)).toBe("0:00");
    expect(setScore(games[0])).toBeNull();
    expect(setScore(games[1])).toBe("1-0");
    const teams = game("dddddddddddd", "2026-10-14T02:00:00Z", {
      isTeams: true,
      players: [
        { uid: "1", displayName: "a", connectCode: "A#1", characterId: 1, costume: 0, teamId: 0, wins: 2 },
        { uid: "2", displayName: "b", connectCode: "B#1", characterId: 1, costume: 0, teamId: 0, wins: 2 },
        { uid: "3", displayName: "c", connectCode: "C#1", characterId: 1, costume: 0, teamId: 1, wins: 1 },
        { uid: "4", displayName: "d", connectCode: "D#1", characterId: 1, costume: 0, teamId: 1, wins: 1 },
      ],
    });
    expect(setScore(teams)).toBe("2-1");
  });
});
