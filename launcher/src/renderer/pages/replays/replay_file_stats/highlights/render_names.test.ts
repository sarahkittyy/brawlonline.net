import type { HighlightPlayer, ReplayHighlight } from "@replays/highlights/types";
import { describe, expect, it } from "vitest";

import { clipVideoName, highlightPlayers } from "./render_names";

const player = (port: number, displayName: string, characterId: number): HighlightPlayer => ({
  port,
  displayName,
  connectCode: "",
  characterId,
  costume: 0,
  teamId: null,
});

const row = (forms?: ReplayHighlight["forms"]): ReplayHighlight => ({
  kind: "highlight",
  attackerPort: 1,
  victimPort: 2,
  firstHitFrame: 100,
  startFrame: 10,
  endFrame: 250,
  kill: false,
  forms,
  score: 1,
});

describe("highlight players", () => {
  // Zelda (0x0e) vs Lucas (0x1b).
  const players = new Map([
    [1, player(1, "amy", 0x0e)],
    [2, player(2, "t3o7", 0x1b)],
  ]);

  it("shows a Zelda who was Sheik in the string as Sheik (bug report #323)", () => {
    const shown = highlightPlayers(players, row([{ port: 1, characterId: 0x0f }]));
    expect(shown.get(1)?.characterId).toBe(0x0f);
    expect(shown.get(2)).toBe(players.get(2));
    // The game's own list is untouched.
    expect(players.get(1)?.characterId).toBe(0x0e);
    expect(highlightPlayers(players, row())).toBe(players);
  });

  it("names a rendered highlight after the form", () => {
    const file = "2026-11-21 2010 game.brep";
    expect(clipVideoName(row([{ port: 1, characterId: 0x0f }]), players, file)).toBe(
      "2026-11-21 2010 amy (Sheik) vs t3o7 (Lucas)",
    );
    expect(clipVideoName(row(), players, file)).toBe("2026-11-21 2010 amy (Zelda) vs t3o7 (Lucas)");
  });
});
