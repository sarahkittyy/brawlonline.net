/**
 * Playing a replay of the Replays tab's list (the user, 2026-10-24): the playback Dolphin then shows
 * "12/340  << >>", the list being the folder's replays as the browser shows them (its filters and
 * sort, all of them, not only the rows loaded so far), and the row playing is shown selected
 * (highlight_nav.ts `REPLAY_LIST`). A replay not in it (opened from a file) plays as before.
 */
import { useCallback, useEffect } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useAccount } from "@/lib/hooks/use_account";
import { buildReplayFilters, useReplayFilter } from "@/lib/hooks/use_replay_filter";
import { useReplays } from "@/lib/hooks/use_replays";
import { useToasts } from "@/lib/hooks/use_toasts";
import {
  REPLAY_LIST,
  replayEntry,
  setPlaylistEntries,
  usePlaylistStore,
} from "@/pages/replays/replay_file_stats/highlights/highlight_nav";
import { useReplayListPlayer } from "@/pages/replays/replay_file_stats/highlights/highlight_playlist";
import { useServices } from "@/services";

/** Every replay of the browser's list, in its order (as Play selected with Select all gets them). */
const useBrowserListPaths = () => {
  const { replayService } = useServices();
  const user = useAccount((store) => store.user);
  return useCallback(async (): Promise<string[]> => {
    const { sortBy, sortDirection, hideShortGames, searchText } = useReplayFilter.getState();
    return await replayService.getAllFilePaths({
      folderPath: useReplays.getState().currentFolder,
      orderBy: {
        field: sortBy === "DATE" ? "startTime" : "lastFrame",
        direction: sortDirection === "DESC" ? "desc" : "asc",
      },
      filters: buildReplayFilters(hideShortGames, searchText, user?.uid),
    });
  }, [replayService, user]);
};

/** Plays `path` with the browser's list as the playback Dolphin's << >> (without, when it isn't in it). */
export const usePlayFromReplayList = () => {
  const { dolphinService } = useServices();
  const { viewReplays } = useDolphinActions(dolphinService);
  const { showError } = useToasts();
  const listPaths = useBrowserListPaths();
  const playInList = useReplayListPlayer();

  return useCallback(
    async (path: string) => {
      let paths: string[] = [];
      try {
        paths = await listPaths();
      } catch (err) {
        showError(err);
      }
      if (paths.includes(path)) {
        await playInList(paths, path);
      } else {
        await viewReplays({ path });
      }
    },
    [listPaths, playInList, viewReplays, showError],
  );
};

/** While a replay of the list plays, another folder, filter or sort is the buttons' list from then on. */
export const useFollowReplayList = () => {
  const { dolphinService } = useServices();
  const listPaths = useBrowserListPaths();
  const currentFolder = useReplays((store) => store.currentFolder);
  const sortBy = useReplayFilter((store) => store.sortBy);
  const sortDirection = useReplayFilter((store) => store.sortDirection);
  const hideShortGames = useReplayFilter((store) => store.hideShortGames);
  const searchText = useReplayFilter((store) => store.searchText);

  useEffect(() => {
    if (usePlaylistStore.getState().source !== REPLAY_LIST) {
      return;
    }
    let live = true;
    listPaths()
      .then((paths) => live && setPlaylistEntries(REPLAY_LIST, paths.map(replayEntry), dolphinService))
      .catch(() => undefined);
    return () => {
      live = false;
    };
  }, [listPaths, dolphinService, currentFolder, sortBy, sortDirection, hideShortGames, searchText]);
};
