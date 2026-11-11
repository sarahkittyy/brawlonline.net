import { ThemeProvider } from "@emotion/react";
import type { GameAssetManifest } from "@game_assets/types";
import type { Theme } from "@mui/material/styles";
import { ThemeProvider as MuiThemeProvider } from "@mui/material/styles";
import log from "electron-log";
import React from "react";
import { create } from "zustand";

import { useThemeColor } from "@/lib/hooks/use_settings";

import {
  applyCss,
  buildPlainThemeCss,
  buildThemeCss,
  framesFromManifest,
  gradientTopColor,
  prepareTheme,
  useGameAssets,
} from "./game_theme";
import { createAppTheme, fallbackPalette, fallbackTheme } from "./theme";
import { normalizeThemeColor, paletteFromThemeColor } from "./theme_color";

/**
 * The theme colour picker's colour while it is open (components/theme_color_picker): the launcher shows it
 * before it is applied. `undefined` = not open or not touched yet (the current look stays).
 */
export const useThemeColorDraft = create<{ draft: string | undefined }>(() => ({ draft: undefined }));

/** The theme colour the launcher shows: the picker's draft, else the setting (null = the default look). */
export function useShownThemeColor(): string | null {
  const [setting] = useThemeColor();
  const draft = useThemeColorDraft((s) => s.draft);
  return normalizeThemeColor(draft ?? setting);
}

/** The default look's colour (the picker's start, the setting's swatch): P+'s gradient top, null for plain grey. */
export const useDefaultThemeColor = create<{ color: string | null }>(() => ({ color: null }));

/**
 * Provides the MUI/emotion theme and keeps it in sync with the extracted game
 * assets and the theme colour. Renders the fallback theme until (and unless) either is there.
 */
export const GameThemeProvider = ({ children }: React.PropsWithChildren) => {
  const [theme, setTheme] = React.useState<Theme>(fallbackTheme);
  const assetsState = useGameAssets((s) => s.state);
  const themeColor = useShownThemeColor();

  React.useEffect(() => {
    const api = window.electron?.gameAssets;
    if (!api) {
      return;
    }
    const setState = useGameAssets.getState().setState;
    void api.getState().then(setState).catch(log.error);
    return api.onStateChange(setState);
  }, []);

  const manifest = assetsState?.manifest ?? null;
  React.useEffect(() => {
    let cancelled = false;
    if (!manifest) {
      useDefaultThemeColor.setState({ color: null });
      return;
    }
    void gradientTopColor(manifest)
      .then((color) => {
        if (!cancelled) {
          useDefaultThemeColor.setState({ color });
        }
      })
      .catch(log.error);
    return () => {
      cancelled = true;
    };
  }, [manifest]);

  // Fonts go on once per manifest, before the frames are tinted (with a theme colour's palette, which needs
  // no textures: no grey moment); the rest also follows the theme colour, which the picker changes as it moves.
  const fontsApplied = React.useRef<GameAssetManifest | null>(null);
  React.useEffect(() => {
    let cancelled = false;
    const palette = themeColor ? paletteFromThemeColor(themeColor) : null;
    if (!manifest) {
      fontsApplied.current = null;
      applyCss(palette ? buildPlainThemeCss(palette) : null);
      setTheme(palette ? createAppTheme(palette) : fallbackTheme);
      return;
    }
    if (fontsApplied.current !== manifest) {
      fontsApplied.current = manifest;
      applyCss(buildThemeCss(manifest, palette ? { palette, images: {} } : null));
    }
    void prepareTheme(manifest, themeColor)
      .then((prepared) => {
        if (cancelled) {
          return;
        }
        applyCss(buildThemeCss(manifest, prepared));
        setTheme(createAppTheme(prepared.palette ?? fallbackPalette, framesFromManifest(manifest, prepared)));
      })
      .catch(log.error);
    return () => {
      cancelled = true;
    };
  }, [manifest, themeColor]);

  return (
    <MuiThemeProvider theme={theme}>
      <ThemeProvider theme={theme as any}>{children}</ThemeProvider>
    </MuiThemeProvider>
  );
};
