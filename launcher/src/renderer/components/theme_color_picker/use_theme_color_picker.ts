import { useCallback } from "react";
import { useNavigate } from "react-router-dom";
import { create } from "zustand";

import { useSettingsStore } from "@/lib/hooks/use_settings";
import { useThemeColorDraft } from "@/styles/game_theme_provider";

/** Where the picker opens (the launcher's main page, to see the colour on it) and where it goes back to. */
export const THEME_COLOR_PICKER_PAGE = "/main/home";
export const THEME_COLOR_SETTINGS_PAGE = "/settings/appearance-settings";

/** Whether the theme colour picker is open over the main page (components/theme_color_picker). */
export const useThemeColorPickerOpen = create<{ open: boolean }>(() => ({ open: false }));

/** Closes the picker: its draft goes, so the launcher shows the setting again. */
export function closeThemeColorPicker() {
  useThemeColorDraft.setState({ draft: undefined });
  useThemeColorPickerOpen.setState({ open: false });
}

/**
 * Saves a theme colour (null = the default look). The renderer's copy changes at once, as favourites do,
 * so dropping the picker's draft right after never shows the old colour for a moment.
 */
export async function saveThemeColor(color: string | null) {
  useSettingsStore.getState().applyUpdates([{ key: "themeColor", value: color }]);
  await window.electron.settings.updateSettings([{ key: "themeColor", value: color }]);
}

/** Opens the picker: Settings closes and the main page shows behind it. */
export function useOpenThemeColorPicker() {
  const navigate = useNavigate();
  return useCallback(() => {
    useThemeColorDraft.setState({ draft: undefined });
    useThemeColorPickerOpen.setState({ open: true });
    navigate(THEME_COLOR_PICKER_PAGE);
  }, [navigate]);
}
