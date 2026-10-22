/**
 * Starred highlights and clips (Replays > Highlights > Favorites): kept in the settings
 * (`favoriteHighlights`) by replay path, kind and start frame, so they outlast the list's order
 * and the highlight cache.
 */
import { useCallback, useMemo } from "react";

import { useSetting, useSettingsStore } from "@/lib/hooks/use_settings";

import type { HighlightEntry } from "./highlight_list";

export const favoriteKey = (entry: HighlightEntry) => `${entry.path}|${entry.row.kind}|${entry.row.startFrame}`;

export const useFavoriteHighlights = () => {
  const [favorites] = useSetting("favoriteHighlights");
  const set = useMemo(() => new Set(favorites), [favorites]);

  const toggle = useCallback((entry: HighlightEntry) => {
    const key = favoriteKey(entry);
    // (From the store, put there at once: a second star before the first is saved keeps both.)
    const now = useSettingsStore.getState().settings.favoriteHighlights;
    const next = now.includes(key) ? now.filter((k) => k !== key) : [...now, key];
    useSettingsStore.getState().applyUpdates([{ key: "favoriteHighlights", value: next }]);
    void window.electron.settings.updateSettings([{ key: "favoriteHighlights", value: next }]);
  }, []);

  return { favorites: set, toggle };
};
