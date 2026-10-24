import type { FileResult, FolderResult, Progress, ReplayService } from "@replays/types";
import { create } from "zustand";
import { immer } from "zustand/middleware/immer";

import { useSettings } from "@/lib/hooks/use_settings";
import { useServices } from "@/services";
import type { AuthService } from "@/services/auth/types";

import { buildReplayFilters, useReplayFilter } from "./use_replay_filter";

const REPLAY_BATCH_SIZE = 50;
const LAST_FOLDER_KEY = "replay-browser-last-folder";

/**
 * Get the initial folder to load in the replay browser.
 * Validates that the stored folder is still a subdirectory of one of the SLP paths.
 * Falls back to rootSlpPath if validation fails.
 */
const getInitialFolder = (): string => {
  const { rootSlpPath, extraSlpPaths } = useSettings.getState().settings;

  // Try to restore the last folder from localStorage
  const storedFolder = localStorage.getItem(LAST_FOLDER_KEY);

  if (storedFolder) {
    // Check if the stored folder is a subdirectory of any of the SLP paths
    const allSlpPaths = [rootSlpPath, ...extraSlpPaths];
    const isValid = allSlpPaths.some((slpPath) => {
      // A folder is valid if it equals the SLP path or is a subdirectory of it
      return storedFolder === slpPath || window.electron.utils.isSubdirectory(slpPath, storedFolder);
    });

    if (isValid) {
      return storedFolder;
    }
  }

  // Fall back to root SLP path if stored folder is invalid or doesn't exist
  return rootSlpPath;
};

type StoreState = {
  loading: boolean;
  loadingMore: boolean;
  progress: Progress | undefined;
  files: FileResult[];
  currentRoot: string | undefined;
  currentFolder: string;
  totalFilesInFolder: number | undefined;
  folderTree: FolderResult[];
  collapsedFolders: string[];
  selectedFile: {
    index: number | undefined;
    total: number | undefined;
    fileResult: FileResult | undefined;
  };
  hasMoreReplays: boolean;
  continuation: string | undefined;
};

const initialState: StoreState = {
  loading: false,
  loadingMore: false,
  progress: undefined,
  files: [],
  folderTree: [],
  collapsedFolders: [],
  currentRoot: undefined,
  currentFolder: getInitialFolder(),
  totalFilesInFolder: undefined,
  selectedFile: {
    index: undefined,
    total: undefined,
    fileResult: undefined,
  },
  hasMoreReplays: false,
  continuation: undefined,
};

export const useReplays = create<StoreState>()(immer(() => initialState));

// Singleton instance of ReplayPresenter
let presenterInstance: ReplayPresenter | undefined = undefined;

export class ReplayPresenter {
  private currentLoadRequestId = 0;

  constructor(private readonly replayService: ReplayService, private readonly authService: AuthService) {}

  async init(rootFolder: string, extraFolders: string[], forceReload?: boolean, currentFolder?: string): Promise<void> {
    const { currentRoot } = useReplays.getState();
    if (currentRoot === rootFolder && !forceReload) {
      return;
    }

    let folderToLoad: string;
    // If the stored folder doesn't exist, just default back to the root folder
    if (currentFolder && (await window.electron.utils.pathExists(currentFolder))) {
      folderToLoad = currentFolder;
    } else {
      folderToLoad = rootFolder;
    }

    const loadFolderList = async () => {
      const folders = [rootFolder, ...extraFolders];
      // Init the folder tree
      await this.replayService.initializeFolderTree(folders);

      // Get the result after folder selection
      const folderTree = await this.replayService.selectTreeFolder(folderToLoad);

      useReplays.setState((state) => {
        state.currentRoot = rootFolder;
        state.folderTree = [...folderTree];
        state.collapsedFolders = [];
      });
    };

    await Promise.all([loadFolderList(), this.loadFolder(folderToLoad, true)]);
  }

  selectFile(file: FileResult, index?: number, total?: number): void {
    useReplays.setState((state) => {
      state.selectedFile = { fileResult: file, index, total };
    });
  }

  clearSelectedFile() {
    useReplays.setState((state) => {
      state.selectedFile = {
        fileResult: undefined,
        index: undefined,
        total: undefined,
      };
    });
  }

  removeFilesByIds(fileIds: string[]) {
    useReplays.setState((state) => {
      state.files = state.files.filter(({ id }) => !fileIds.includes(id));
    });
  }

  updateProgress(progress: { current: number; total: number } | undefined) {
    useReplays.setState((state) => {
      state.progress = progress;
    });
  }

  async loadFolder(childPath?: string, forceReload?: boolean): Promise<void> {
    // Increment request ID for this load operation
    const requestId = ++this.currentLoadRequestId;
    const { currentFolder } = useReplays.getState();

    const folderToLoad = childPath ?? currentFolder;

    // Persist the folder to localStorage for next session
    localStorage.setItem(LAST_FOLDER_KEY, folderToLoad);

    useReplays.setState((state) => {
      state.currentFolder = folderToLoad;
      state.loadingMore = false;
    });

    const loadFolderTree = async () => {
      const folderTree = await this.replayService.selectTreeFolder(folderToLoad);

      // Check if this request is still current before updating state
      if (this.currentLoadRequestId !== requestId) {
        return;
      }

      useReplays.setState((state) => {
        state.folderTree = [...folderTree];
      });
    };

    const loadFolderDetails = async () => {
      // (A load still running for it is superseded by this one, which must finish it.)
      if (currentFolder === folderToLoad && !forceReload && !useReplays.getState().loading) {
        console.warn(`${currentFolder} is already loaded. Set forceReload to true to reload anyway.`);
        return;
      }

      useReplays.setState((state) => {
        state.loading = true;
        state.loadingMore = false;
        state.progress = undefined;
      });
      try {
        // Get current filter state
        const { sortBy, sortDirection, hideShortGames, searchText } = useReplayFilter.getState();

        // Build filters from current state
        const currentUserId = this.authService.getCurrentUser()?.uid;
        const filters = buildReplayFilters(hideShortGames, searchText, currentUserId);

        // Use searchGames with pagination - load first batch
        const result = await this.replayService.searchGames({
          folderPath: folderToLoad,
          limit: REPLAY_BATCH_SIZE,
          orderBy: {
            field: sortBy === "DATE" ? "startTime" : "lastFrame",
            direction: sortDirection === "DESC" ? "desc" : "asc",
          },
          filters,
        });

        // Check if this request is still current before updating state
        if (this.currentLoadRequestId !== requestId) {
          return;
        }

        useReplays.setState((state) => {
          state.files = result.files;
          state.loading = false;
          state.continuation = result.continuation;
          state.hasMoreReplays = result.continuation != null;
          state.totalFilesInFolder = result.totalCount;
        });
      } catch (err) {
        // Only update state if this is still the current request
        if (this.currentLoadRequestId === requestId) {
          useReplays.setState((state) => {
            state.loading = false;
            state.progress = undefined;
          });
        }
      }
    };
    await Promise.all([loadFolderTree(), loadFolderDetails()]);
  }

  toggleFolder(folder: string) {
    useReplays.setState((state) => {
      if (state.collapsedFolders.includes(folder)) {
        state.collapsedFolders = state.collapsedFolders.filter((f) => f !== folder);
      } else {
        state.collapsedFolders = [...state.collapsedFolders, folder];
      }
    });
  }

  async loadMoreReplays(): Promise<void> {
    const { continuation, loading, loadingMore, hasMoreReplays, currentFolder } = useReplays.getState();

    // Don't load more if already loading or no more replays. While a folder (re)load runs, the list and its
    // continuation are the old one's, and that load lists the first page anyway.
    if (loading || loadingMore || !hasMoreReplays || !continuation) {
      return;
    }
    // A page belongs to the folder load that listed the ones before it: a newer load drops it. It does not
    // supersede that load itself, whose result would be dropped with `loading` left on (bug report #118).
    const requestId = this.currentLoadRequestId;

    useReplays.setState((state) => {
      state.loadingMore = true;
    });

    try {
      // Get current filter state
      const { sortBy, sortDirection, hideShortGames, searchText } = useReplayFilter.getState();

      // Build filters from current state
      const currentUserId = this.authService.getCurrentUser()?.uid;
      const filters = buildReplayFilters(hideShortGames, searchText, currentUserId);

      // Load next batch of replays
      const result = await this.replayService.searchGames({
        folderPath: currentFolder,
        limit: REPLAY_BATCH_SIZE,
        continuation,
        orderBy: {
          field: sortBy === "DATE" ? "startTime" : "lastFrame",
          direction: sortDirection === "DESC" ? "desc" : "asc",
        },
        filters,
      });

      // Check if this request is still current before updating state
      if (this.currentLoadRequestId !== requestId) {
        return;
      }

      useReplays.setState((state) => {
        state.files = [...state.files, ...result.files];
        state.continuation = result.continuation;
        state.hasMoreReplays = result.continuation !== undefined;
        state.loadingMore = false;
      });
    } catch (err) {
      console.error("Failed to load more replays:", err);
      // Only update state if this is still the current request
      if (this.currentLoadRequestId === requestId) {
        useReplays.setState((state) => {
          state.loadingMore = false;
        });
      }
    }
  }
}

/**
 * Get the singleton instance of ReplayPresenter.
 * This ensures only one presenter instance exists across the entire application.
 */
export const getReplayPresenter = (replayService: ReplayService, authService: AuthService): ReplayPresenter => {
  if (!presenterInstance) {
    presenterInstance = new ReplayPresenter(replayService, authService);
  }
  return presenterInstance;
};

/**
 * Hook to get the singleton ReplayPresenter instance.
 * Use this instead of creating new ReplayPresenter instances directly.
 */
export const useReplayPresenter = () => {
  const { replayService, authService } = useServices();
  return getReplayPresenter(replayService, authService);
};
