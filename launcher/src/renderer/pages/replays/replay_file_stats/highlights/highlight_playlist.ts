/**
 * Playing a list's highlight, and answering the playback Dolphin's list buttons and stars
 * (highlight_nav.ts has the list and where each button goes).
 */
import { DolphinEventType, DolphinLaunchType } from "@dolphin/types";
import { useCallback, useEffect } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useServices } from "@/services";

import type { PlaylistEntry } from "./highlight_nav";
import {
  followStatus,
  listView,
  newCommandId,
  REPLAY_LIST,
  replayEntry,
  replayListNav,
  usePlaylistStore,
} from "./highlight_nav";

/** Plays an entry of a list in the playback Dolphin, that list then being the buttons'. Resolves to whether it was asked. */
export const useHighlightPlayer = () => {
  const { dolphinService } = useServices();
  const { playHighlights } = useDolphinActions(dolphinService);

  return useCallback(
    async (source: string, entries: PlaylistEntry[], entry: PlaylistEntry): Promise<boolean> => {
      const commandId = newCommandId();
      usePlaylistStore.setState({ source, entries, current: entry, commandId, navSeq: 0, pickSeq: 0 });
      const item = { path: entry.path, startFrame: entry.startFrame, endFrame: entry.endFrame };
      const ok = await playHighlights([item], commandId, listView(entries, item));
      if (!ok && usePlaylistStore.getState().commandId === commandId) {
        usePlaylistStore.setState({ current: null, commandId: null });
      }
      return ok;
    },
    [playHighlights],
  );
};

/** Plays a replay of the Replays tab's list (`paths`, in its order), that list then being the buttons'. */
export const useReplayListPlayer = () => {
  const { dolphinService } = useServices();
  const { playReplayInList } = useDolphinActions(dolphinService);

  return useCallback(
    async (paths: readonly string[], path: string): Promise<boolean> => {
      const entries = paths.map(replayEntry);
      const commandId = newCommandId();
      usePlaylistStore.setState({
        source: REPLAY_LIST,
        entries,
        current: replayEntry(path),
        commandId,
        navSeq: 0,
        pickSeq: 0,
      });
      const ok = await playReplayInList(path, { commandId, nav: replayListNav(entries, path) });
      if (!ok && usePlaylistStore.getState().commandId === commandId) {
        usePlaylistStore.setState({ current: null, commandId: null });
      }
      return ok;
    },
    [playReplayInList],
  );
};

/** Answers the playback Dolphin's list buttons and stars; mounted once for the whole window. */
export const useHighlightNavigation = () => {
  const { dolphinService } = useServices();
  const playHighlight = useHighlightPlayer();
  const playReplay = useReplayListPlayer();
  const play = useCallback(
    (source: string, entries: PlaylistEntry[], entry: PlaylistEntry) =>
      source === REPLAY_LIST
        ? playReplay(
            entries.map((e) => e.path),
            entry.path,
          )
        : playHighlight(source, entries, entry),
    [playHighlight, playReplay],
  );

  useEffect(
    () =>
      dolphinService.onEvent(DolphinEventType.PLAYBACK_STATUS, ({ status }) =>
        followStatus(status, (source, entries, entry) => void play(source, entries, entry)),
      ),
    [dolphinService, play],
  );

  // Dolphin closed (not to start again on another release's card): nothing plays.
  useEffect(
    () =>
      dolphinService.onEvent(DolphinEventType.CLOSED, (event) => {
        if (
          event.dolphinType === DolphinLaunchType.PLAYBACK &&
          "instanceId" in event &&
          event.instanceId === "playback" &&
          !event.replaced
        ) {
          usePlaylistStore.setState({ current: null, commandId: null });
        }
      }),
    [dolphinService],
  );
};
