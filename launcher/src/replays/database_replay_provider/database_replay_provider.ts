import { chunk } from "@common/chunk";
import type { ReplayFilter } from "@database/filters/types";
import { FileRepository, inFolderTree } from "@database/repositories/file_repository";
import { GameRepository } from "@database/repositories/game_repository";
import { PlayerRepository } from "@database/repositories/player_repository";
import type { Database, FileRecord, GameRecord, NewFile, NewGame, NewPlayer, PlayerRecord } from "@database/schema";
import { boolToInt, boolToIntOrNull } from "@database/utils";
import type { FileResult, GameEnding, MatchReplay, Progress, ReplayProvider } from "@replays/types";
import { STATS_NOT_AVAILABLE } from "@replays/types";
import type { StadiumStatsType, StatsType } from "@slippi/slippi-js/node";
import { shell } from "electron";
import log from "electron-log";
import { pathExists } from "fs-extra";
import type { Kysely, Transaction } from "kysely";
import { readdir, stat } from "node:fs/promises";
import path from "path";

import { extractPlayerNames } from "../extract_player_names";
import type { ReplayEnd } from "../replay_format";
import { readReplayFileInfo, readReplayHeader } from "../replay_format";
import type { ParsedFileInfo } from "../replay_indexing_pool/replay_indexing.worker.interface";
import type { ReplayIndexingPoolManager } from "../replay_indexing_pool/replay_indexing_pool_manager";
import { Continuation } from "./continuation";
import { inferStartTime } from "./infer_start_time";
import { mapGameRecordToFileResult } from "./record_mapper";

// A folder's view includes its subfolders (the replay folder's YYYY-MM folders): how deep and how
// many folders are indexed with it at most
const SUBFOLDER_DEPTH = 4;
const MAX_FOLDERS = 256;

// Batch size for processing replays - how many insertions to be done in a single transaction
const INSERT_REPLAY_BATCH_SIZE = 500;

// SQLite has a limit of 999 parameters per query (SQLITE_MAX_VARIABLE_NUMBER)
// We use a conservative batch size to stay well under this limit for DELETE and IN operations
const SQLITE_PARAM_LIMIT_BATCH_SIZE = 500;

export class DatabaseReplayProvider implements ReplayProvider {
  private currentSearchRequestId = 0;
  /** loadFile calls in progress by path: the stats page's two calls load the same file at once. */
  private readonly loadingFiles = new Map<string, Promise<FileResult>>();
  /** findReplaysForMatches' game endings by path, while the file keeps its size and time. */
  private readonly endings = new Map<string, { key: string; ending: GameEnding }>();

  constructor(private readonly db: Kysely<Database>, private readonly workerPool?: ReplayIndexingPoolManager) {}

  /**
   * Check if a request ID has been superseded by a newer request.
   * @param requestId - The request ID to check (undefined means no tracking)
   * @returns true if the request is stale and should be cancelled
   */
  private isRequestSuperseded(requestId: number | undefined): boolean {
    return requestId !== undefined && this.currentSearchRequestId !== requestId;
  }

  async searchReplays(
    folder: string | undefined,
    limit: number,
    continuation?: string,
    orderBy: {
      field: "lastFrame" | "startTime";
      direction?: "asc" | "desc";
    } = {
      field: "startTime",
      direction: "desc",
    },
    filters: ReplayFilter[] = [],
    onProgress?: (progress: Progress) => void,
  ): Promise<{
    files: FileResult[];
    continuation: string | undefined;
    totalCount?: number;
  }> {
    // Increment request ID for this search
    const requestId = ++this.currentSearchRequestId;
    const searchStartTime = Date.now();
    log.info(`[Request ${requestId}] Starting searchReplays for folder: ${folder ?? "all"}`);

    const maybeContinuationToken = Continuation.fromString(continuation);
    const continuationValue = maybeContinuationToken?.getValue() ?? null;
    const nextIdInclusive = maybeContinuationToken?.getNextIdInclusive() ?? null;

    // Sync the database first (add new files, remove deleted files)
    // Only sync if we're not using continuation (i.e., first page of results) and folder is specified
    if (folder !== undefined && continuationValue === null && nextIdInclusive === null) {
      // Only sync if the folder exists on disk
      if (await pathExists(folder)) {
        const syncStartTime = Date.now();
        await this.syncFolderTree(folder, onProgress, requestId);
        const syncDuration = Date.now() - syncStartTime;
        log.info(`[Request ${requestId}] Database sync completed in ${syncDuration}ms`);

        if (this.isRequestSuperseded(requestId)) {
          return {
            files: [],
            continuation: undefined,
            totalCount: 0,
          };
        }
      }
    }

    // Get total count on first page load (when there's no continuation)
    let totalCount: number | undefined;
    if (continuationValue === null && nextIdInclusive === null) {
      const countStartTime = Date.now();
      totalCount = await GameRepository.countGames(this.db, folder ?? null, filters);
      const countDuration = Date.now() - countStartTime;
      log.info(`[Request ${requestId}] Count query completed in ${countDuration}ms (found ${totalCount} games)`);

      if (this.isRequestSuperseded(requestId)) {
        return {
          files: [],
          continuation: undefined,
          totalCount: 0,
        };
      }
    }

    // Convert continuation value from string to proper type
    let typedContinuationValue: string | number | null = null;
    if (continuationValue !== null) {
      if (continuationValue === "null") {
        typedContinuationValue = null;
      } else if (orderBy.field === "lastFrame") {
        typedContinuationValue = parseInt(continuationValue, 10);
      } else {
        typedContinuationValue = continuationValue;
      }
    }

    // Use GameRepository.searchGames which supports filters
    const queryStartTime = Date.now();
    const records = await GameRepository.searchGames(this.db, folder ?? null, filters, {
      limit: limit + 1,
      orderBy: {
        field: orderBy.field,
        direction: orderBy.direction ?? "desc",
      },
      continuationValue: typedContinuationValue,
      nextIdInclusive: nextIdInclusive ?? undefined,
    });
    const queryDuration = Date.now() - queryStartTime;
    log.info(
      `[Request ${requestId}] Search query completed in ${queryDuration}ms (returned ${records.length} records)`,
    );

    if (this.isRequestSuperseded(requestId)) {
      return {
        files: [],
        continuation: undefined,
        totalCount: 0,
      };
    }

    const [recordsToReturn, newContinuation] = Continuation.truncate(records, limit, (record) => ({
      value: orderBy.field === "startTime" ? record.start_time ?? "null" : record.last_frame?.toString() ?? "null",
      nextIdInclusive: record._id,
    }));

    const mapStartTime = Date.now();
    const files = await this.mapGameAndFileRecordsToFileResult(recordsToReturn);
    const mapDuration = Date.now() - mapStartTime;
    log.info(`[Request ${requestId}] Player mapping completed in ${mapDuration}ms`);

    const totalDuration = Date.now() - searchStartTime;
    log.info(`[Request ${requestId}] Total searchReplays completed in ${totalDuration}ms`);

    return {
      files,
      continuation: newContinuation,
      totalCount,
    };
  }

  async findReplaysForMatches(matchIds: string[], folders: string[]): Promise<Record<string, MatchReplay[]>> {
    // The replay folders and their month folders (Dolphin's ReplayMonthlyFolders), indexed first:
    // only files the database has can be found by match.
    for (const folder of folders.slice(0, 16)) {
      if (await pathExists(folder)) {
        await this.syncFolderTree(folder);
      }
    }
    const found: Record<string, MatchReplay[]> = {};
    for (const matchId of matchIds.slice(0, 200)) {
      if (!matchId) {
        continue;
      }
      const records = await GameRepository.getAllFilePaths(
        this.db,
        null,
        [{ type: "session", sessionIds: [matchId] }],
        { field: "startTime", direction: "asc" },
      );
      if (records.length > 0) {
        found[matchId] = await Promise.all(
          records.map(async (record) => {
            const file = path.resolve(record.folder, record.name);
            return { path: file, ending: await this.gameEnding(file) };
          }),
        );
      }
    }
    return found;
  }

  /** How a replay's game ended (its ENDG), read again only when the file changed. */
  private async gameEnding(file: string): Promise<GameEnding> {
    let key = "";
    try {
      const info = await stat(file);
      key = `${info.size}:${info.mtimeMs}`;
      const known = this.endings.get(file);
      if (known && known.key === key) {
        return known.ending;
      }
      const ending = gameEndingOf((await readReplayHeader(file)).end);
      this.endings.set(file, { key, ending });
      return ending;
    } catch {
      // Gone or not readable as a replay: no result to show.
      return "unfinished";
    }
  }

  async getAllFilePaths(
    folder: string | undefined,
    orderBy: {
      field: "lastFrame" | "startTime";
      direction?: "asc" | "desc";
    } = {
      field: "startTime",
      direction: "desc",
    },
    filters: ReplayFilter[] = [],
  ): Promise<string[]> {
    // Sync the database first (add new files, remove deleted files)
    // Only sync if folder is specified and exists on disk
    if (folder !== undefined && (await pathExists(folder))) {
      await this.syncFolderTree(folder);
    }

    // Query all file paths using the dedicated repository method
    const records = await GameRepository.getAllFilePaths(this.db, folder ?? null, filters, {
      field: orderBy.field,
      direction: orderBy.direction ?? "desc",
    });

    // Return just the full paths
    return records.map((record) => path.resolve(record.folder, record.name));
  }

  async loadFile(filePath: string): Promise<FileResult> {
    // A file not indexed yet is inserted by the first call; a second call at the same time
    // would insert it again (SQLITE_CONSTRAINT_UNIQUE), so it waits for the first one.
    const inProgress = this.loadingFiles.get(filePath);
    if (inProgress) {
      return await inProgress;
    }
    const loading = this.loadFileOnce(filePath).finally(() => this.loadingFiles.delete(filePath));
    this.loadingFiles.set(filePath, loading);
    return await loading;
  }

  private async loadFileOnce(filePath: string): Promise<FileResult> {
    const filename = path.basename(filePath);
    const folder = path.dirname(filePath);

    let playerRecords: PlayerRecord[];
    let gameAndFileRecord = await GameRepository.findGameByFolderAndFilename(this.db, folder, filename);

    if (gameAndFileRecord) {
      const playerMap = await PlayerRepository.findAllPlayersByGame(this.db, gameAndFileRecord._id);
      playerRecords = playerMap.get(gameAndFileRecord._id) ?? [];
    } else {
      // We haven't indexed this file before so add it to the database
      const replay = await this.insertNewReplayFile(folder, filename);
      if (!replay.gameRecord) {
        throw new Error(`Could not load game info from file ${replay.fileRecord._id} at path: ${filePath}`);
      }
      gameAndFileRecord = { ...replay.fileRecord, ...replay.gameRecord };
      playerRecords = replay.playerRecords;
    }

    return mapGameRecordToFileResult(gameAndFileRecord, playerRecords);
  }

  async calculateGameStats(_fullPath: string): Promise<StatsType | undefined> {
    // Slippi computes stats from .slp files with slippi-js; our replay format has no parser yet.
    throw new Error(STATS_NOT_AVAILABLE);
  }

  async calculateStadiumStats(_fullPath: string): Promise<StadiumStatsType | undefined> {
    return undefined;
  }

  async deleteReplays(fileIds: string[]): Promise<void> {
    const deleteStartTime = Date.now();
    log.info(`Deleting ${fileIds.length} replay(s) by ID`);

    // Convert string IDs to numbers
    const numericFileIds = fileIds.map((id) => parseInt(id, 10));

    // Get the file records to get the file paths
    const fileRecords = await this.queryFileRecordsByIds(numericFileIds);

    if (fileRecords.length === 0) {
      log.warn("No files found for the given IDs");
      return;
    }

    // Delete the records from the database first (cascades to game and player records)
    await this.deleteFileRecordsFromDatabase(numericFileIds);

    // Delete files from the filesystem
    await this.deleteFilesFromFilesystem(fileRecords);

    const totalDuration = Date.now() - deleteStartTime;
    log.info(`Deleted ${fileRecords.length} replay(s) in ${totalDuration}ms total`);
  }

  async bulkDeleteReplays(
    folder: string | undefined,
    filters: ReplayFilter[] = [],
    options?: {
      excludeFilePaths?: string[];
    },
  ): Promise<{ deletedCount: number }> {
    const bulkDeleteStartTime = Date.now();
    log.info(
      `Starting bulk delete with ${filters.length} filter(s) and ${options?.excludeFilePaths?.length ?? 0} exclusions`,
    );

    // Sync the database first to ensure we're working with up-to-date data
    // Only sync if folder is specified and exists on disk
    if (folder !== undefined && (await pathExists(folder))) {
      const syncStartTime = Date.now();
      await this.syncFolderTree(folder);
      const syncDuration = Date.now() - syncStartTime;
      log.info(`Pre-delete sync completed in ${syncDuration}ms`);
    }

    // Convert file paths to file IDs for exclusion
    // Batch the query to stay under SQLite's 999 parameter limit
    const excludeFileIds: number[] = [];
    if (options?.excludeFilePaths && options.excludeFilePaths.length > 0) {
      const excludeQueryStartTime = Date.now();
      const excludeFilenames = options.excludeFilePaths.map((p) => path.basename(p));
      const chunkedFilenames = chunk(excludeFilenames, SQLITE_PARAM_LIMIT_BATCH_SIZE);

      for (const batch of chunkedFilenames) {
        let excludeQuery = this.db.selectFrom("file");

        // Apply folder filter if specified
        if (folder !== undefined) {
          excludeQuery = excludeQuery.where(inFolderTree(folder));
        }

        const excludeRecords = await excludeQuery.where("name", "in", batch).select("_id").execute();
        excludeFileIds.push(...excludeRecords.map((r) => r._id));
      }

      const excludeQueryDuration = Date.now() - excludeQueryStartTime;
      log.info(`Exclude query completed in ${excludeQueryDuration}ms (found ${excludeFileIds.length} exclusions)`);
    }

    // Get all file IDs matching the filters
    const filterQueryStartTime = Date.now();
    const fileIdsToDelete = await GameRepository.getFileIdsForBulkDelete(
      this.db,
      folder ?? null,
      filters,
      excludeFileIds.length > 0 ? excludeFileIds : undefined,
    );
    const filterQueryDuration = Date.now() - filterQueryStartTime;
    log.info(`Filter query completed in ${filterQueryDuration}ms (found ${fileIdsToDelete.length} files to delete)`);

    if (fileIdsToDelete.length === 0) {
      log.info("No files to delete");
      return { deletedCount: 0 };
    }

    // Get the file records to get the file paths
    const fileRecords = await this.queryFileRecordsByIds(fileIdsToDelete);

    // Delete the records from the database first (cascades to game and player records)
    await this.deleteFileRecordsFromDatabase(fileIdsToDelete);

    // Delete files from the filesystem
    await this.deleteFilesFromFilesystem(fileRecords);

    const totalDuration = Date.now() - bulkDeleteStartTime;
    log.info(`Bulk deleted ${fileRecords.length} replay(s) in ${totalDuration}ms total`);
    return { deletedCount: fileRecords.length };
  }

  private async mapGameAndFileRecordsToFileResult(records: (GameRecord & FileRecord)[]): Promise<FileResult[]> {
    const gameIds = records.map((game) => game._id);
    const playerMap = await PlayerRepository.findAllPlayersByGame(this.db, ...gameIds);
    return records.map((gameRecord): FileResult => {
      const players = playerMap.get(gameRecord._id) ?? [];
      return mapGameRecordToFileResult(gameRecord, players);
    });
  }

  /**
   * Query file records by IDs with automatic chunking to stay under SQLite parameter limits.
   * @param fileIds - Array of file IDs to query
   * @returns Array of file records
   */
  private async queryFileRecordsByIds(fileIds: number[]): Promise<FileRecord[]> {
    const queryStartTime = Date.now();
    const chunkedFileIds = chunk(fileIds, SQLITE_PARAM_LIMIT_BATCH_SIZE);
    const fileRecords: FileRecord[] = [];

    for (const batch of chunkedFileIds) {
      const batchRecords = await this.db.selectFrom("file").where("file._id", "in", batch).selectAll().execute();
      fileRecords.push(...batchRecords);
    }

    const queryDuration = Date.now() - queryStartTime;
    log.info(`File record query completed in ${queryDuration}ms (found ${fileRecords.length} records)`);

    return fileRecords;
  }

  /**
   * Delete file records from database by IDs with automatic chunking.
   * Cascades to game and player records.
   * @param fileIds - Array of file IDs to delete
   */
  private async deleteFileRecordsFromDatabase(fileIds: number[]): Promise<void> {
    const dbDeleteStartTime = Date.now();
    const chunkedDeleteIds = chunk(fileIds, SQLITE_PARAM_LIMIT_BATCH_SIZE);

    for (const batch of chunkedDeleteIds) {
      await FileRepository.deleteFileById(this.db, ...batch);
    }

    const dbDeleteDuration = Date.now() - dbDeleteStartTime;
    log.info(`Database delete completed in ${dbDeleteDuration}ms (deleted ${fileIds.length} records)`);
  }

  /**
   * Delete files from filesystem by moving them to trash.
   * @param fileRecords - Array of file records to delete from filesystem
   * @returns Number of files that failed to delete
   */
  private async deleteFilesFromFilesystem(fileRecords: FileRecord[]): Promise<number> {
    const fsDeleteStartTime = Date.now();
    const filePaths = fileRecords.map((record) => path.resolve(record.folder, record.name));
    const deletePromises = await Promise.allSettled(filePaths.map((filePath) => shell.trashItem(filePath)));
    const errCount = deletePromises.reduce((curr, { status }) => (status === "rejected" ? curr + 1 : curr), 0);
    const fsDeleteDuration = Date.now() - fsDeleteStartTime;

    if (errCount > 0) {
      log.warn(`${errCount} file(s) failed to delete from filesystem in ${fsDeleteDuration}ms`);
    } else {
      log.info(`Filesystem delete completed in ${fsDeleteDuration}ms`);
    }

    return errCount;
  }

  /**
   * Indexes `folder` and the folders under it (SUBFOLDER_DEPTH deep, MAX_FOLDERS at most), and
   * forgets the files of folders under it that are gone from disk.
   */
  private async syncFolderTree(folder: string, onProgress?: (progress: Progress) => void, requestId?: number) {
    const folders = [folder];
    const depth = [0];
    for (let i = 0; i < folders.length && folders.length < MAX_FOLDERS; i++) {
      if (depth[i] >= SUBFOLDER_DEPTH) {
        continue;
      }
      try {
        const entries = await readdir(folders[i], { withFileTypes: true });
        for (const e of entries) {
          if (e.isDirectory() && folders.length < MAX_FOLDERS) {
            folders.push(path.join(folders[i], e.name));
            depth.push(depth[i] + 1);
          }
        }
      } catch (err) {
        log.warn(`Could not list ${folders[i]}: ${err}`);
      }
    }
    for (const f of folders) {
      if (this.isRequestSuperseded(requestId)) {
        return;
      }
      await this.syncReplayDatabase(f, f === folder ? onProgress : undefined, INSERT_REPLAY_BATCH_SIZE, requestId);
    }
    const listed = new Set(folders);
    for (const gone of await FileRepository.findFoldersUnder(this.db, folder)) {
      if (!listed.has(gone) && !(await pathExists(gone))) {
        const files = await FileRepository.findAllFilesInFolder(this.db, gone);
        for (const batch of chunk(
          files.map(({ _id }) => _id),
          SQLITE_PARAM_LIMIT_BATCH_SIZE,
        )) {
          await FileRepository.deleteFileById(this.db, ...batch);
        }
        log.info(`Forgot ${files.length} replay(s) of ${gone} (no longer on disk)`);
      }
    }
  }

  private async syncReplayDatabase(
    folder: string,
    onProgress?: (progress: Progress) => void,
    batchSize = 100,
    requestId?: number,
  ) {
    const syncStartTime = Date.now();

    // Start fetching existing files from database
    const existingFilesPromise = FileRepository.findAllFilesInFolder(this.db, folder);

    // Use streaming to discover .slp files (non-blocking)
    // This avoids blocking the main thread when scanning large directories
    const slpFileNames: string[] = [];
    let totalScanned = 0;

    const { streamSlpFiles } = await import("../streaming_folder_service");
    const streamGen = streamSlpFiles(folder, 200);

    for await (const progress of streamGen) {
      totalScanned = progress.scanned;
      slpFileNames.length = 0;
      slpFileNames.push(...progress.slpFiles);

      // Log intermediate progress for large directories
      if (!progress.complete && totalScanned % 1000 === 0) {
        log.info(`Scanned ${totalScanned} entries, found ${slpFileNames.length} .slp files so far...`);
      }
    }

    // Wait for existing files query to complete
    const existingFiles = await existingFilesPromise;

    const reqLog = requestId ? `[Request ${requestId}] ` : "";
    log.info(
      `${reqLog}Found ${slpFileNames.length} .slp files on disk (scanned ${totalScanned} entries), ${existingFiles.length} files in database`,
    );

    // Find all records in the database that no longer exist
    const deleteOldReplays = async () => {
      const deleteStartTime = Date.now();
      const setOfSlpFileNames = new Set(slpFileNames);
      const fileIdsToDelete = existingFiles.filter(({ name }) => !setOfSlpFileNames.has(name)).map(({ _id }) => _id);

      if (fileIdsToDelete.length === 0) {
        log.info("No replays to delete");
        return;
      }

      log.info(`Deleting ${fileIdsToDelete.length} replays that no longer exist on disk`);
      const chunkedFileIdsToDelete = chunk(fileIdsToDelete, batchSize);
      let deletedCount = 0;

      for (const batch of chunkedFileIdsToDelete) {
        const batchStartTime = Date.now();
        await FileRepository.deleteFileById(this.db, ...batch);
        deletedCount += batch.length;
        const batchDuration = Date.now() - batchStartTime;
        log.info(
          `Deleted batch of ${batch.length} replays in ${batchDuration}ms (${deletedCount}/${fileIdsToDelete.length})`,
        );
      }

      const deleteDuration = Date.now() - deleteStartTime;
      log.info(`Completed deleting ${fileIdsToDelete.length} replays in ${deleteDuration}ms`);
    };

    // Find all new SLP files that are not yet in the database
    const insertNewReplays = async () => {
      const insertStartTime = Date.now();
      const setOfExistingReplayNames = new Set(existingFiles.map((r) => r.name));
      const slpFilesToAdd = slpFileNames.filter((name) => !setOfExistingReplayNames.has(name));
      const total = slpFilesToAdd.length;

      if (total === 0) {
        log.info("No new replays to add");
        return;
      }

      log.info(`Adding ${total} new replays to database`);
      let replaysAdded = 0;
      const chunkedReplays = chunk(slpFilesToAdd, batchSize);

      // Process each batch in a single transaction for optimal performance
      // This reduces transaction overhead from N transactions to 1 per batch
      for (const batch of chunkedReplays) {
        if (this.isRequestSuperseded(requestId)) {
          onProgress?.({ current: replaysAdded, total });
          return;
        }

        const batchStartTime = Date.now();

        // Parse files using managed worker pool (if available) before transaction
        // Pool is created on-demand and will auto-shutdown after idle timeout
        // This allows parallel parsing off the main thread across multiple workers
        let parsedFiles: Map<string, ParsedFileInfo>;
        if (this.workerPool) {
          const parseStartTime = Date.now();
          const parseResults = await this.workerPool.parseReplayFileBatch(folder, batch);
          const parseDuration = Date.now() - parseStartTime;
          const filesPerSec = Math.round((batch.length / parseDuration) * 1000);
          const poolSize = this.workerPool.getPoolSize();
          const poolInfo = poolSize > 0 ? ` across ${poolSize} workers` : "";
          log.info(
            `Worker pool parsed ${batch.length} files in ${parseDuration}ms (${filesPerSec} files/sec${poolInfo})`,
          );

          parsedFiles = new Map(parseResults.filter((r) => r.success && r.data).map((r) => [r.filename, r.data!]));

          const parseFailures = parseResults.filter((r) => !r.success);
          if (parseFailures.length > 0) {
            log.warn(`Failed to parse ${parseFailures.length} replay(s): `, parseFailures[0].error);
          }
        } else {
          parsedFiles = new Map();
        }

        // Insert parsed files into database within a transaction
        const batchResults = await this.db.transaction().execute(async (trx) => {
          const results: Array<{ success: boolean; error?: any }> = [];

          // Process files sequentially within the transaction
          // SQLite is single-writer, so parallel processing just creates lock contention
          for (const filename of batch) {
            if (this.isRequestSuperseded(requestId)) {
              break;
            }

            try {
              // Use pre-parsed data if available, otherwise parse on demand
              const parsedInfo = parsedFiles.get(filename) ?? (await this.parseReplayFile(folder, filename));
              const newFile = generateNewFile(folder, parsedInfo);
              const fileRecord = await FileRepository.insertFile(trx, newFile);

              const newGame = generateNewGame(fileRecord, parsedInfo);
              if (newGame) {
                const gameRecord = await GameRepository.insertGame(trx, newGame);
                const newPlayers = generateNewPlayers(gameRecord._id, parsedInfo);
                await PlayerRepository.insertPlayer(trx, ...newPlayers);
              }

              results.push({ success: true });
            } catch (err) {
              results.push({ success: false, error: err });
            }
          }

          return results;
        });

        const successful = batchResults.filter((r) => r.success).length;
        const failed = batchResults.filter((r) => !r.success);

        replaysAdded += successful;
        const batchDuration = Date.now() - batchStartTime;
        const filesPerSec = Math.round((successful / batchDuration) * 1000);
        log.info(`Added ${replaysAdded}/${total} replays in ${batchDuration}ms (${filesPerSec} files/sec)`);

        if (failed.length > 0) {
          log.warn(`Failed to add ${failed.length} replay(s): `, failed[0].error);
        }

        onProgress?.({ current: replaysAdded, total });
      }

      const insertDuration = Date.now() - insertStartTime;
      const totalFilesPerSec = Math.round((replaysAdded / insertDuration) * 1000);
      log.info(`Completed adding ${replaysAdded} replays in ${insertDuration}ms (avg ${totalFilesPerSec} files/sec)`);

      onProgress?.({ current: total, total });
    };

    // Run delete and insert sequentially for better performance
    // SQLite is single-writer, so running these in parallel just adds overhead
    // and lock contention without any benefit
    try {
      await deleteOldReplays();
    } catch (err) {
      log.warn("Error removing deleted replays: " + err);
    }

    try {
      await insertNewReplays();
    } catch (err) {
      throw new Error("Error inserting new replays: " + err);
    }

    const syncDuration = Date.now() - syncStartTime;
    log.info(`Total sync operation completed in ${syncDuration}ms`);
  }

  /**
   * Parse a replay file using the managed worker pool if available, otherwise fallback to main thread.
   * The pool will be created on-demand and automatically shut down after idle timeout.
   * @param folder - The folder containing the replay file
   * @param filename - The name of the replay file
   * @returns The parsed file information
   */
  private async parseReplayFile(folder: string, filename: string): Promise<ParsedFileInfo> {
    if (this.workerPool) {
      const result = await this.workerPool.parseReplayFile(folder, filename);
      if (!result.success) {
        throw new Error(`Failed to parse replay file ${filename}: ${result.error}`);
      }
      return result.data!;
    } else {
      // Fallback to main thread parsing
      return await readReplayFileInfo(folder, filename);
    }
  }

  /**
   * Insert a new replay file within an existing transaction.
   * This is the core implementation used by both batch processing and single-file operations.
   *
   * @param trx - The transaction to use for the insert
   * @param folder - The folder containing the replay file
   * @param filename - The name of the replay file
   * @returns The inserted file, game, and player records
   */
  private async insertNewReplayFileInTransaction(
    trx: Transaction<Database>,
    folder: string,
    filename: string,
  ): Promise<{ fileRecord: FileRecord; gameRecord: GameRecord | undefined; playerRecords: PlayerRecord[] }> {
    const parsedInfo = await this.parseReplayFile(folder, filename);
    const newFile = generateNewFile(folder, parsedInfo);

    const fileRecord = await FileRepository.insertFile(trx, newFile);

    const newGame = generateNewGame(fileRecord, parsedInfo);
    if (!newGame) {
      return { fileRecord, gameRecord: undefined, playerRecords: [] };
    }

    const gameRecord = await GameRepository.insertGame(trx, newGame);
    const newPlayers = generateNewPlayers(gameRecord._id, parsedInfo);
    // A replay listed by name and date only has no players (an insert of none is an SQL error).
    const playerRecords = newPlayers.length > 0 ? await PlayerRepository.insertPlayer(trx, ...newPlayers) : [];

    return { fileRecord, gameRecord, playerRecords };
  }

  /**
   * Insert a new replay file (creates its own transaction).
   * This method is used for single-file operations like loading a file that isn't in the database.
   *
   * @param folder - The folder containing the replay file
   * @param filename - The name of the replay file
   * @returns The inserted file, game, and player records
   */
  private async insertNewReplayFile(
    folder: string,
    filename: string,
  ): Promise<{ fileRecord: FileRecord; gameRecord: GameRecord | undefined; playerRecords: PlayerRecord[] }> {
    return await this.db.transaction().execute(async (trx) => {
      return await this.insertNewReplayFileInTransaction(trx, folder, filename);
    });
  }
}

function generateNewFile(folder: string, parsedInfo: ParsedFileInfo): NewFile {
  const newReplay: NewFile = {
    name: parsedInfo.filename,
    folder,
    size_bytes: parsedInfo.sizeBytes,
    birth_time: parsedInfo.birthTime,
  };
  return newReplay;
}

function generateNewGame(file: FileRecord, parsedInfo: ParsedFileInfo): NewGame | null {
  // Load settings
  const settings = parsedInfo.settings;
  if (!settings || settings.players.length === 0) {
    // Our replay format has no parser yet (replay_format.ts): list the file with
    // only its date, so the browser shows it instead of skipping it.
    return {
      file_id: file._id,
      is_ranked: boolToInt(false),
      is_teams: boolToInt(false),
      stage: null,
      start_time: inferStartTime(null, file.name, file.birth_time),
      platform: null,
      console_nickname: null,
      mode: null,
      last_frame: null,
      timer_type: null,
      starting_timer_secs: null,
      session_id: null,
    };
  }
  const metadata = parsedInfo.metadata;

  const sessionId = settings.matchInfo?.sessionId ?? null;
  const isRanked = sessionId != null && sessionId.startsWith("mode.ranked-");

  const gameStartTime = inferStartTime(metadata?.startAt ?? null, file.name, file.birth_time);

  const newGame: NewGame = {
    file_id: file._id,
    is_ranked: boolToInt(isRanked),
    is_teams: boolToInt(settings.isTeams ?? false),
    stage: settings.stageId,
    start_time: gameStartTime,
    platform: metadata?.playedOn,
    console_nickname: metadata?.consoleNick,
    mode: settings.gameMode,
    last_frame: metadata?.lastFrame,
    timer_type: settings.timerType,
    starting_timer_secs: settings.startingTimerSeconds,
    session_id: sessionId,
    game_number: settings.matchInfo?.gameNumber ?? undefined,
    tiebreak_number: settings.matchInfo?.tiebreakerNumber ?? undefined,
    pplus_release: parsedInfo.pplusRelease ?? null,
  };

  return newGame;
}

function generateNewPlayers(gameId: number, parsedInfo: ParsedFileInfo): NewPlayer[] {
  const settings = parsedInfo.settings;
  if (!settings || settings.players.length === 0) {
    return [];
  }

  const winnerIndices = parsedInfo.winnerIndices;
  return settings.players.map((player): NewPlayer => {
    const isWinner = winnerIndices.includes(player.playerIndex);
    const names = extractPlayerNames(player.playerIndex, settings, parsedInfo.metadata);
    const newPlayer: NewPlayer = {
      game_id: gameId,
      port: player.playerIndex + 1,
      type: player.type,
      character_id: player.characterId,
      character_color: player.characterColor,
      team_id: settings.isTeams ? player.teamId : undefined,
      is_winner: boolToIntOrNull(isWinner),
      start_stocks: player.startStocks,
      connect_code: names.code,
      display_name: names.name,
      tag: names.tag,
      user_id: player.userId,
    };
    return newPlayer;
  });
}

/** A replay's ENDG as the match history counts it (GameEnding). */
export function gameEndingOf(end: ReplayEnd | null): GameEnding {
  if (!end) {
    return "unfinished";
  }
  if (end.winnerPort != null) {
    return "won";
  }
  if (end.draw) {
    return "draw";
  }
  switch (end.endMethod) {
    case "quit":
    case "quit-left":
    case "runback":
      return "quit";
    case "desync":
      return "desync";
    default:
      return "unfinished";
  }
}
