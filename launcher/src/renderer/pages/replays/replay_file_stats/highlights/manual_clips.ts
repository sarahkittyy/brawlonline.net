/**
 * Manual clips (docs/design/highlights.md §9): the playback Dolphin's seek bar clips that the
 * launcher keeps in the settings (`manualClips`, src/replays/highlights/manual_clips.ts), as the
 * lists' rows; a row's remove button takes one out.
 */
import type { ManualClip } from "@replays/highlights/types";
import { useCallback } from "react";

import { useSetting, useSettingsStore } from "@/lib/hooks/use_settings";

import type { HighlightEntry } from "./highlight_list";

const clipKey = (path: string, startFrame: number, endFrame: number) => `${path}|${startFrame}|${endFrame}`;

/** A manual clip as a row: its game is what was kept of it (players, teams, recorder). */
export const manualClipEntry = (clip: ManualClip): HighlightEntry => ({
  key: `manual:${clipKey(clip.path, clip.startFrame, clip.endFrame)}`,
  path: clip.path,
  game: {
    detectorVersion: 0,
    players: clip.players,
    isTeams: clip.isTeams,
    recorderPort: clip.recorderPort,
    highlights: [],
    clips: [],
  },
  row: { kind: "manual", startFrame: clip.startFrame, endFrame: clip.endFrame, savedAt: clip.savedAt },
  date: clip.date,
});

export const useManualClips = () => {
  const [clips] = useSetting("manualClips");

  const remove = useCallback((entry: HighlightEntry) => {
    const key = clipKey(entry.path, entry.row.startFrame, entry.row.endFrame);
    // (From the store, put there at once, as favorites.ts does.)
    const now = useSettingsStore.getState().settings.manualClips;
    const next = now.filter((c) => clipKey(c.path, c.startFrame, c.endFrame) !== key);
    useSettingsStore.getState().applyUpdates([{ key: "manualClips", value: next }]);
    void window.electron.settings.updateSettings([{ key: "manualClips", value: next }]);
  }, []);

  return { clips, remove };
};
