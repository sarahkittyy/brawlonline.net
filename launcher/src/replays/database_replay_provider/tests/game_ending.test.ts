import { describe, expect, it } from "vitest";

import type { ReplayEnd } from "../../replay_format";
import { gameEndingOf } from "../database_replay_provider";

const end = (endMethod: string, winnerPort: number | null = null, draw = false): ReplayEnd => ({
  endMethod,
  firstFrame: 0,
  lastFrame: 100,
  finalFrame: 100,
  winnerPort,
  draw,
  players: [],
});

describe("gameEndingOf", () => {
  it("counts a game with a winner as won, a Ranked quit's too", () => {
    expect(gameEndingOf(end("game", 1))).toBe("won");
    expect(gameEndingOf(end("quit", 2))).toBe("won");
  });

  it("tells a draw, a quit or runback and a desync apart", () => {
    expect(gameEndingOf(end("game", null, true))).toBe("draw");
    expect(gameEndingOf(end("quit"))).toBe("quit");
    expect(gameEndingOf(end("quit-left"))).toBe("quit");
    expect(gameEndingOf(end("runback"))).toBe("quit");
    expect(gameEndingOf(end("desync"))).toBe("desync");
  });

  it("calls the rest unfinished: a disconnect, a stop, a file without its end", () => {
    expect(gameEndingOf(end("disconnect"))).toBe("unfinished");
    expect(gameEndingOf(end("stopped"))).toBe("unfinished");
    expect(gameEndingOf(null)).toBe("unfinished");
  });
});
