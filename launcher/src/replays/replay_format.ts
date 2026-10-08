/**
 * Our replay format is not decided yet (PPLUS_PORTING.md, "Replays"). Until it
 * is, the replay browser lists files with these extensions by name, size and
 * date only: no players, stage or stats, which Slippi reads from `.slp` files
 * with slippi-js. This module is the one place to plug in a real parser.
 */
import { stat } from "node:fs/promises";
import path from "path";

/** Planned replay files: `.rep` (binary) and `.json` (debug/export). */
export const REPLAY_EXTENSIONS: readonly string[] = [".rep", ".json"];

export function isReplayFileName(fileName: string): boolean {
  return REPLAY_EXTENSIONS.includes(path.extname(fileName).toLowerCase());
}

/** What the indexer stores about a replay file (Slippi's `ParsedFileInfo` shape). */
export type MinimalReplayInfo = {
  filename: string;
  sizeBytes: number;
  birthTime: string | undefined;
  settings: undefined;
  metadata: undefined;
  winnerIndices: number[];
};

export async function readReplayFileInfo(folder: string, filename: string): Promise<MinimalReplayInfo> {
  let sizeBytes = 0;
  let birthTime: string | undefined = undefined;
  try {
    const info = await stat(path.join(folder, filename));
    sizeBytes = info.size;
    birthTime = info.birthtime.toISOString();
  } catch {
    // Keep the defaults; the file is still listed.
  }
  return { filename, sizeBytes, birthTime, settings: undefined, metadata: undefined, winnerIndices: [] };
}
