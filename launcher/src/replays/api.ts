/* eslint-disable import/no-default-export */
import type { AllHighlightsEvent } from "./highlights/types";
import {
  ipc_allHighlightsEvent,
  ipc_bulkDeleteReplays,
  ipc_calculateGameStats,
  ipc_calculateStadiumStats,
  ipc_deleteReplays,
  ipc_findReplaysForMatches,
  ipc_getAllFilePaths,
  ipc_getAllHighlights,
  ipc_getReplayHighlights,
  ipc_initializeFolderTree,
  ipc_loadProgressUpdatedEvent,
  ipc_searchGames,
  ipc_selectTreeFolder,
  ipc_statsPageRequestedEvent,
} from "./ipc";
import type { BulkDeleteOptions, Progress, ReplayService, SearchGamesOptions } from "./types";

const replayApi: ReplayService = {
  async initializeFolderTree(folders: readonly string[]) {
    const { result } = await ipc_initializeFolderTree.renderer!.trigger({ folders });
    return result;
  },
  async selectTreeFolder(folderPath: string) {
    const { result } = await ipc_selectTreeFolder.renderer!.trigger({ folderPath });
    return result;
  },
  async searchGames(options: SearchGamesOptions) {
    const { result } = await ipc_searchGames.renderer!.trigger(options);
    return result;
  },
  async getAllFilePaths(options: SearchGamesOptions) {
    const { result } = await ipc_getAllFilePaths.renderer!.trigger(options);
    return result;
  },
  async calculateGameStats(filePath: string) {
    const { result } = await ipc_calculateGameStats.renderer!.trigger({ filePath });
    return result;
  },
  async calculateStadiumStats(filePath: string) {
    const { result } = await ipc_calculateStadiumStats.renderer!.trigger({ filePath });
    return result;
  },
  async deleteReplays(fileIds: string[]) {
    await ipc_deleteReplays.renderer!.trigger({ fileIds });
  },
  async bulkDeleteReplays(options: BulkDeleteOptions) {
    const { result } = await ipc_bulkDeleteReplays.renderer!.trigger(options);
    return result;
  },
  async findReplaysForMatches(matchIds: string[], folders: string[]) {
    const { result } = await ipc_findReplaysForMatches.renderer!.trigger({ matchIds, folders });
    return result;
  },
  async getHighlights(filePath: string) {
    const { result } = await ipc_getReplayHighlights.renderer!.trigger({ filePath });
    return result;
  },
  async getAllHighlights() {
    const { result } = await ipc_getAllHighlights.renderer!.trigger({});
    return result;
  },
  onAllHighlights(handle: (event: AllHighlightsEvent) => void) {
    const { destroy } = ipc_allHighlightsEvent.renderer!.handle(async (event) => {
      handle(event);
    });
    return destroy;
  },
  onReplayLoadProgressUpdate(handle: (progress: Progress) => void) {
    const { destroy } = ipc_loadProgressUpdatedEvent.renderer!.handle(async (progress) => {
      handle(progress);
    });
    return destroy;
  },
  onStatsPageRequest(handle: (filePath: string) => void) {
    const { destroy } = ipc_statsPageRequestedEvent.renderer!.handle(async ({ filePath }) => {
      handle(filePath);
    });
    return destroy;
  },
};

export default replayApi;
