import type { AllHighlightsEvent, GameHighlights, ReplayHighlights } from "@replays/highlights/types";
import type {
  BulkDeleteOptions,
  BulkDeleteResult,
  FileResult,
  FolderResult,
  Progress,
  ReplayService,
  SearchGamesOptions,
  SearchGamesResult,
} from "@replays/types";
import type { StadiumStatsType, StatsType } from "@slippi/slippi-js";

import { delayAndMaybeError } from "../utils";
import { aMockFileResultWith, aMockFolderResultWith } from "./mocks";

const SHOULD_ERROR = false;

class MockReplayClient implements ReplayService {
  @delayAndMaybeError(SHOULD_ERROR)
  async initializeFolderTree(folders: readonly string[]): Promise<readonly FolderResult[]> {
    return folders.map((folder) => aMockFolderResultWith(folder));
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async selectTreeFolder(folderPath: string): Promise<readonly FolderResult[]> {
    return ["foo", "bar", "baz"].map((name) => aMockFolderResultWith(folderPath, { name }));
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async searchGames(options: SearchGamesOptions): Promise<SearchGamesResult> {
    const folderPath = options.folderPath ?? "/default/folder";
    const files = [1, 2, 3, 4].map((i) => aMockFileResultWith(folderPath, { fileName: `Game${i}.slp` }));
    return {
      files,
      continuation: undefined,
    };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async getAllFilePaths(options: SearchGamesOptions): Promise<string[]> {
    const folderPath = options.folderPath ?? "/default/folder";
    return [1, 2, 3, 4].map((i) => `${folderPath}/Game${i}.slp`);
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async calculateGameStats(_filePath: string): Promise<{ file: FileResult; stats: StatsType | undefined }> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async calculateStadiumStats(
    _filePath: string,
  ): Promise<{ file: FileResult; stadiumStats: StadiumStatsType | undefined }> {
    throw new Error("Method not implemented.");
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async deleteReplays(_fileIds: string[]): Promise<void> {
    return;
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async bulkDeleteReplays(_options: BulkDeleteOptions): Promise<BulkDeleteResult> {
    return { deletedCount: 4 };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async findReplaysForMatches(_matchIds: string[], _folders: string[]): Promise<Record<string, string[]>> {
    return {};
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async getHighlights(_filePath: string): Promise<ReplayHighlights> {
    const player = (port: number, displayName: string, characterId: number) => ({
      port,
      displayName,
      connectCode: "",
      characterId,
      costume: 0,
      teamId: null,
    });
    const highlight = (attackerPort: number, victimPort: number, frame: number, score: number) => ({
      kind: "highlight" as const,
      attackerPort,
      victimPort,
      firstHitFrame: frame,
      startFrame: frame - 90,
      endFrame: frame + 400,
      kill: score > 15,
      score,
    });
    return {
      detectorVersion: 2,
      players: [player(1, "sarahkitty", 2), player(2, "Smear", 22)],
      isTeams: false,
      highlights: [highlight(1, 2, 4200, 19.3), highlight(2, 1, 1800, 15.2), highlight(1, 2, 9100, 12.1)],
      clips: [{ kind: "clip", port: 1, frame: 6000, startFrame: 4770, endFrame: 6120 }],
    };
  }

  @delayAndMaybeError(SHOULD_ERROR)
  async getAllHighlights(): Promise<{ games: GameHighlights[]; pending: number; total: number }> {
    const highlights = await this.getHighlights("");
    const path = "C:/Replays/2026-10-15 2153 Ranked G1 - sarahkitty (Link) vs Smear (Snake).brep";
    return { games: [{ path, date: "2026-10-15T21:53:00.000Z", highlights }], pending: 0, total: 1 };
  }

  onAllHighlights(_handle: (event: AllHighlightsEvent) => void): () => void {
    return () => void 0;
  }

  onReplayLoadProgressUpdate(_handle: (progress: Progress) => void): () => void {
    return () => void 0;
  }

  onStatsPageRequest(_handle: (filePath: string) => void): () => void {
    return () => void 0;
  }
}

export default function createMockReplayClient(): ReplayService {
  return new MockReplayClient();
}
