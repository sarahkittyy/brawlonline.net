import { createDatabase } from "@database/create_database";
import type { DolphinManager } from "@dolphin/manager";
import { DolphinEventType } from "@dolphin/types";
import type { SettingsManager } from "@settings/settings_manager";
import { app } from "electron";
import log from "electron-log";
import path from "path";

import { DatabaseReplayProvider } from "./database_replay_provider/database_replay_provider";
import { FolderTreeService } from "./folder_tree_service";
import { AllHighlights } from "./highlights/all_highlights";
import { HighlightService } from "./highlights/highlight_service";
import { ManualClips } from "./highlights/manual_clips";
import {
  ipc_allHighlightsEvent,
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
} from "./ipc";
import { createManagedReplayIndexingPool } from "./replay_indexing_pool/replay_indexing_pool_manager";
import type { Progress } from "./types";

const REPLAY_DATABASE_NAME = "replays.sqlite";

const isDevelopment = process.env.NODE_ENV === "development";

async function createReplayProvider() {
  const replayDatabaseFolder = path.join(app.getPath("userData"), REPLAY_DATABASE_NAME);

  // Create the managed replay indexing pool (lazy initialization + idle timeout)
  // Workers will be created on-demand and automatically shut down after 5 minutes of inactivity
  let workerPoolManager;
  try {
    workerPoolManager = createManagedReplayIndexingPool();
    log.info("Replay indexing pool manager created (workers will be created on-demand)");
  } catch (err) {
    log.error(`Failed to create replay indexing pool manager: ${err}. Will use main thread for parsing.`);
  }

  // Register cleanup on app quit
  if (workerPoolManager) {
    app.on("quit", async () => {
      log.info("App quitting, terminating worker pool...");
      await workerPoolManager.terminate();
    });
  }

  try {
    const database = await createDatabase(isDevelopment ? undefined : replayDatabaseFolder);
    return { database, provider: new DatabaseReplayProvider(database, workerPoolManager) };
  } catch (err) {
    log.error(
      `Failed to initialize replay database at ${replayDatabaseFolder}: ${err}. Using in-memory database instead.`,
    );
    const database = await createDatabase(undefined);
    return { database, provider: new DatabaseReplayProvider(database, workerPoolManager) };
  }
}

export default function setupReplaysIpc({
  settingsManager,
  dolphinManager,
}: {
  settingsManager: SettingsManager;
  dolphinManager: DolphinManager;
}) {
  const treeService = new FolderTreeService();
  const created = createReplayProvider();
  const replayProviderPromise = created.then(({ provider }) => provider);
  const highlightService = new HighlightService(
    created.then(({ database }) => database),
    () => {
      const { disableHighlightDetection, clipOnLrz } = settingsManager.get().settings;
      return { detectHighlights: !disableHighlightDetection, clipOnLrz };
    },
  );

  const allHighlights = new AllHighlights(
    highlightService,
    () => {
      const { rootSlpPath, extraSlpPaths } = settingsManager.get().settings;
      return [rootSlpPath, ...extraSlpPaths];
    },
    (event) => void ipc_allHighlightsEvent.main!.trigger(event).catch(log.warn),
  );

  // The playback Dolphin's seek bar clips (Clip, then "Save as highlight"): Replays > Highlights > Manual Clips.
  const manualClips = new ManualClips({
    clips: () => settingsManager.get().settings.manualClips,
    save: (clips) => settingsManager.updateSetting("manualClips", clips),
    spectateDir: () => dolphinManager.spectateDir(),
    replayDir: () => settingsManager.getRootSlpPath(),
  });
  dolphinManager.events.subscribe((event) => {
    if (event.type === DolphinEventType.PLAYBACK_STATUS) {
      manualClips.onStatus(event.status);
    }
  });

  ipc_getAllHighlights.main!.handle(async () => {
    return await allHighlights.list();
  });

  ipc_getReplayHighlights.main!.handle(async ({ filePath }) => {
    return await highlightService.getHighlights(filePath);
  });

  ipc_initializeFolderTree.main!.handle(async ({ folders }) => {
    return treeService.init(folders);
  });

  ipc_selectTreeFolder.main!.handle(async ({ folderPath }) => {
    return await treeService.select(folderPath);
  });

  ipc_calculateGameStats.main!.handle(async ({ filePath }) => {
    const replayProvider = await replayProviderPromise;
    const [stats, fileResult] = await Promise.all([
      replayProvider.calculateGameStats(filePath),
      replayProvider.loadFile(filePath),
    ]);
    return { file: fileResult, stats };
  });

  ipc_calculateStadiumStats.main!.handle(async ({ filePath }) => {
    const replayProvider = await replayProviderPromise;
    const [stadiumStats, fileResult] = await Promise.all([
      replayProvider.calculateStadiumStats(filePath),
      replayProvider.loadFile(filePath),
    ]);
    return { file: fileResult, stadiumStats };
  });

  ipc_searchGames.main!.handle(async (options) => {
    const replayProvider = await replayProviderPromise;
    const { limit = 20, continuation, orderBy = { field: "startTime", direction: "desc" }, filters } = options;

    // Progress callback for database sync
    const onProgress = (progress: Progress) => {
      ipc_loadProgressUpdatedEvent.main!.trigger(progress).catch(console.warn);
    };

    return await replayProvider.searchReplays(options.folderPath, limit, continuation, orderBy, filters, onProgress);
  });

  ipc_getAllFilePaths.main!.handle(async (options) => {
    const replayProvider = await replayProviderPromise;
    const { orderBy = { field: "startTime", direction: "desc" }, filters } = options;

    return await replayProvider.getAllFilePaths(options.folderPath, orderBy, filters);
  });

  ipc_deleteReplays.main!.handle(async ({ fileIds }) => {
    const replayProvider = await replayProviderPromise;
    await replayProvider.deleteReplays(fileIds);
    return { success: true };
  });

  ipc_findReplaysForMatches.main!.handle(async ({ matchIds, folders }) => {
    const replayProvider = await replayProviderPromise;
    return await replayProvider.findReplaysForMatches(matchIds, folders);
  });
}
