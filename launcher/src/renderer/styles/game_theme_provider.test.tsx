import { useTheme } from "@mui/material/styles";
import { act, cleanup, render } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// The settings store reads the main process's settings when it loads (use_settings.ts). No game assets.
vi.hoisted(() => {
  (window as any).electron = {
    settings: {
      getAppSettingsSync: () => ({ previousVersion: null, settings: { themeColor: null } }),
      updateSettings: async () => undefined,
    },
  };
});

import { useSettingsStore } from "@/lib/hooks/use_settings";

import { GameThemeProvider, useThemeColorDraft } from "./game_theme_provider";
import { fallbackPalette } from "./theme";
import { paletteFromThemeColor } from "./theme_color";

let muiPrimary = "";
const Probe = () => {
  muiPrimary = useTheme().palette.primary.main;
  return null;
};

const cssPrimary = () =>
  /--accent-primary: (#[0-9a-f]{6});/.exec(document.getElementById("game-theme")?.textContent ?? "")?.[1];

const RED = "#d33c3c";
const BLUE = "#3c64d3";

describe("GameThemeProvider and the theme colour picker", () => {
  afterEach(() => {
    cleanup();
    vi.useRealTimers();
  });

  beforeEach(() => {
    vi.useFakeTimers();
    act(() => useSettingsStore.getState().applyUpdates([{ key: "themeColor", value: null }]));
    useThemeColorDraft.setState({ draft: undefined });
  });

  it("moves the CSS with every pick and the MUI theme only once the colour rests", () => {
    render(
      <GameThemeProvider>
        <Probe />
      </GameThemeProvider>,
    );
    expect(muiPrimary).toBe(fallbackPalette.primary);

    act(() => useThemeColorDraft.setState({ draft: RED }));
    expect(cssPrimary()).toBe(paletteFromThemeColor(RED).primary);
    expect(muiPrimary).toBe(fallbackPalette.primary);

    act(() => {
      vi.advanceTimersByTime(100);
    });
    act(() => useThemeColorDraft.setState({ draft: BLUE }));
    expect(cssPrimary()).toBe(paletteFromThemeColor(BLUE).primary);
    act(() => {
      vi.advanceTimersByTime(100);
    });
    // Still moving: red never reached the MUI theme.
    expect(muiPrimary).toBe(fallbackPalette.primary);

    act(() => {
      vi.advanceTimersByTime(60);
    });
    expect(muiPrimary).toBe(paletteFromThemeColor(BLUE).primary);
  });

  it("follows the setting at once when the picker closes", () => {
    render(
      <GameThemeProvider>
        <Probe />
      </GameThemeProvider>,
    );
    act(() => useThemeColorDraft.setState({ draft: BLUE }));
    act(() => {
      useSettingsStore.getState().applyUpdates([{ key: "themeColor", value: RED }]);
      useThemeColorDraft.setState({ draft: undefined });
    });
    expect(cssPrimary()).toBe(paletteFromThemeColor(RED).primary);
    expect(muiPrimary).toBe(paletteFromThemeColor(RED).primary);
  });
});
