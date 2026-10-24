// Rendered videos' file names (DESIGN.md §5: descriptive): the game's date and time from its
// replay's name, then who did what, e.g. "2026-10-20 1635 sarahkitty (Mr. Game & Watch) vs t3o7
// (Lucas)". The render queue makes them safe for the file system and unique.
import { STOCK_CHARACTERS } from "@brawl_assets/catalog";
import type { HighlightPlayer, ReplayClip, ReplayHighlight, ReplayManualClip } from "@replays/highlights/types";

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

/** A highlight's, an L+R+Z clip's or a manual clip's video ("... Clip - sarahkitty (Snake) vs t3o7 (Lucas)"). */
export function clipVideoName(
  row: ReplayHighlight | ReplayClip | ReplayManualClip,
  players: Map<number, HighlightPlayer>,
  filePath: string,
): string {
  const date = replayDateOf(filePath);
  const what =
    row.kind === "highlight"
      ? `${who(players.get(row.attackerPort))} vs ${who(players.get(row.victimPort))}`
      : row.kind === "manual"
      ? `Clip - ${[...players.values()].map(who).join(" vs ")}`
      : `Clip by ${who(players.get(row.port))}`;
  return [date, what].filter(Boolean).join(" ");
}

/** A combo movie of `count` highlights of one game. */
export function comboVideoName(players: HighlightPlayer[], count: number, filePath: string): string {
  const date = replayDateOf(filePath);
  const game = players.map((p) => who(p)).join(" vs ");
  return [date, game, `- combo movie (${count})`].filter(Boolean).join(" ");
}

/** A combo movie of `count` highlights from several games: named after when it was made. */
export function multiGameComboName(count: number, made: Date): string {
  const pad = (n: number) => String(n).padStart(2, "0");
  const stamp = `${made.getFullYear()}-${pad(made.getMonth() + 1)}-${pad(made.getDate())} ${pad(made.getHours())}${pad(
    made.getMinutes(),
  )}`;
  return `${stamp} Combo movie (${count} highlights)`;
}
