import { useCallback } from "react";
import { useNavigate } from "react-router-dom";
import { create } from "zustand";

import { useSettingsStore } from "@/lib/hooks/use_settings";
import { useThemeColorDraft } from "@/styles/game_theme_provider";

/** Where the picker opens (the launcher's main page, to see the colour on it) and where it goes back to. */
export const THEME_COLOR_PICKER_PAGE = "/main/home";
export const THEME_COLOR_SETTINGS_PAGE = "/settings/appearance-settings";

/**
 * Whether the theme colour picker is open over the main page (components/theme_color_picker), and the page
 * Cancel/Apply go back to: Settings > Appearance when it was opened there, none from the header's palette
 * button (the picker just closes over the page it was opened on).
 */
export const useThemeColorPickerOpen = create<{ open: boolean; returnTo: string | null }>(() => ({
  open: false,
  returnTo: null,
}));

/** Closes the picker: its draft goes, so the launcher shows the setting again. */
export function closeThemeColorPicker() {
  useThemeColorDraft.setState({ draft: undefined });
  useThemeColorPickerOpen.setState({ open: false, returnTo: null });
}

/**
 * Saves a theme colour (null = the default look). The renderer's copy changes at once, as favourites do,
 * so dropping the picker's draft right after never shows the old colour for a moment.
 */
export async function saveThemeColor(color: string | null) {
  useSettingsStore.getState().applyUpdates([{ key: "themeColor", value: color }]);
  await window.electron.settings.updateSettings([{ key: "themeColor", value: color }]);
}

/** Opens the picker from Settings: Settings closes, Home shows behind it, and closing it goes back to Settings. */
export function useOpenThemeColorPicker() {
  const navigate = useNavigate();
  return useCallback(() => {
    useThemeColorDraft.setState({ draft: undefined });
    useThemeColorPickerOpen.setState({ open: true, returnTo: THEME_COLOR_SETTINGS_PAGE });
    navigate(THEME_COLOR_PICKER_PAGE);
  }, [navigate]);
}

/** Opens the picker over the current page (the header's palette button); closing it stays there. */
export function openThemeColorPickerHere() {
  useThemeColorDraft.setState({ draft: undefined });
  useThemeColorPickerOpen.setState({ open: true, returnTo: null });
}
