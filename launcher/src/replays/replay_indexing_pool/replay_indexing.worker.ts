// NOTE: This module cannot use electron-log, since it for some reason
// fails to obtain the paths required for file transport to work
// when in Node worker context.

import type { GameStartType, MetadataType } from "@slippi/slippi-js/node";
import type { ModuleMethods } from "threads/dist/types/master";
import { expose } from "threads/worker";

import { readReplayFileInfo } from "../replay_format";

/**
 * Parsed file information returned from worker
 */
export interface ParsedFileInfo {
  filename: string;
  sizeBytes: number;
  birthTime: string | undefined;
  settings: GameStartType | undefined;
  metadata: MetadataType | undefined;
  winnerIndices: number[];
}

/**
 * Result of parsing a file - either success or error
 */
export interface ParseFileResult {
  success: boolean;
  filename: string;
  data?: ParsedFileInfo;
  error?: string;
}

interface Methods {
  dispose: () => Promise<void>;
  parseReplayFile(folder: string, filename: string): Promise<ParseFileResult>;
  parseReplayFileBatch(folder: string, filenames: string[]): Promise<ParseFileResult[]>;
}

export type WorkerSpec = ModuleMethods & Methods;

const methods: WorkerSpec = {
  async dispose(): Promise<void> {
    // Worker cleanup if needed
  },

  async parseReplayFile(folder: string, filename: string): Promise<ParseFileResult> {
    try {
      // Our replay format is not decided yet: list the file with what the filesystem knows.
      const data = await readReplayFileInfo(folder, filename);
      return { success: true, filename, data };
    } catch (err) {
      return {
        success: false,
        filename,
        error: err instanceof Error ? err.message : String(err),
      };
    }
  },

  async parseReplayFileBatch(folder: string, filenames: string[]): Promise<ParseFileResult[]> {
    const results: ParseFileResult[] = [];

    for (const filename of filenames) {
      const result = await methods.parseReplayFile(folder, filename);
      results.push(result);
    }

    return results;
  },
};

expose(methods);
