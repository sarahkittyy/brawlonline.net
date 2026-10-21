// Rendered videos' file names (DESIGN.md §5: descriptive): the game's date and time from its
// replay's name, then who did what, e.g. "2026-10-20 1635 sarahkitty (Mr. Game & Watch) vs t3o7
// (Lucas)". The render queue makes them safe for the file system and unique.
import { STOCK_CHARACTERS } from "@brawl_assets/catalog";
import type { HighlightPlayer, ReplayClip, ReplayHighlight } from "@replays/highlights/types";

import { characterStockKeys } from "@/lib/utils";

/** "2026-10-20 1635" from a replay file named the way ours are, else "". */
export function replayDateOf(filePath: string): string {
  const base = filePath.split(/[\\/]/).pop() ?? "";
  return /^(\d{4}-\d{2}-\d{2} \d{4})/.exec(base)?.[1] ?? "";
}

export function characterName(characterId: number): string {
  const key = characterStockKeys[characterId];
  return STOCK_CHARACTERS.find((c) => c.key === key)?.name ?? "";
}

function who(player: HighlightPlayer | undefined): string {
  if (!player) {
    return "";
  }
  const name = player.displayName || player.connectCode || `P${player.port}`;
  const character = characterName(player.characterId);
  return character ? `${name} (${character})` : name;
}

/** A highlight's or an L+R+Z clip's video. */
export function clipVideoName(
  row: ReplayHighlight | ReplayClip,
  players: Map<number, HighlightPlayer>,
  filePath: string,
): string {
  const date = replayDateOf(filePath);
  const what =
    row.kind === "highlight"
      ? `${who(players.get(row.attackerPort))} vs ${who(players.get(row.victimPort))}`
      : `Clip by ${who(players.get(row.port))}`;
  return [date, what].filter(Boolean).join(" ");
}

/** A combo movie of `count` highlights of one game. */
export function comboVideoName(players: HighlightPlayer[], count: number, filePath: string): string {
  const date = replayDateOf(filePath);
  const game = players.map((p) => who(p)).join(" vs ");
  return [date, game, `- combo movie (${count})`].filter(Boolean).join(" ");
}
