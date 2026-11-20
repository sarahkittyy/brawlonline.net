import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { MemoryRouter, Route, Routes, useLocation, useNavigate } from "react-router-dom";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

// The settings store reads the main process's settings when it loads (use_settings.ts).
const updateSettings = vi.hoisted(() => {
  const update = vi.fn(async (_updates: { key: string; value: unknown }[]) => undefined);
  (window as any).electron = {
    settings: {
      getAppSettingsSync: () => ({ previousVersion: null, settings: { themeColor: null } }),
      updateSettings: update,
    },
  };
  return update;
});

import { useSettingsStore } from "@/lib/hooks/use_settings";
import { ThemeColorSetting } from "@/pages/settings/appearance_settings/theme_color/theme_color";
import { useShownThemeColor, useThemeColorDraft } from "@/styles/game_theme_provider";
import { DEFAULT_THEME_COLOR } from "@/styles/theme_color";

import { ThemeColorButton } from "./theme_color_button";
import { ThemeColorPicker } from "./theme_color_picker";
import { THEME_COLOR_PICKER_PAGE, THEME_COLOR_SETTINGS_PAGE, useThemeColorPickerOpen } from "./use_theme_color_picker";

let shown = "";
let path = "";
let navigateTo: (to: string) => void = () => undefined;
const Probe = () => {
  shown = useShownThemeColor();
  path = useLocation().pathname;
  navigateTo = useNavigate();
  return null;
};

const renderApp = (at: string) =>
  render(
    <MemoryRouter initialEntries={[at]}>
      <Routes>
        <Route path="/settings/*" element={<ThemeColorSetting />} />
        <Route
          path="/main/*"
          element={
            <div>
              main page <ThemeColorButton />
            </div>
          }
        />
      </Routes>
      <ThemeColorPicker />
      <Probe />
    </MemoryRouter>,
  );

const setThemeColor = (value: string | null) =>
  act(() => useSettingsStore.getState().applyUpdates([{ key: "themeColor", value }]));

describe("Theme Color", () => {
  afterEach(cleanup);

  beforeEach(() => {
    updateSettings.mockClear();
    setThemeColor(null);
    useThemeColorPickerOpen.setState({ open: false, returnTo: null });
    useThemeColorDraft.setState({ draft: undefined });
  });

  it("shows PM's blue, the default, with no reset while no colour is chosen", () => {
    renderApp(THEME_COLOR_SETTINGS_PAGE);
    const swatch = screen.getByRole("button", { name: "Change theme color" });
    expect(DEFAULT_THEME_COLOR).toBe("#5463c8");
    expect(swatch.style.background).toBe("rgb(84, 99, 200)");
    expect(shown).toBe(DEFAULT_THEME_COLOR);
    expect(screen.queryByRole("button", { name: "Reset to default" })).toBeNull();
  });

  it("offers a reset once a colour is chosen, and it goes back to the default", async () => {
    setThemeColor("#d33c3c");
    renderApp(THEME_COLOR_SETTINGS_PAGE);
    expect(screen.getByRole("button", { name: "Change theme color" }).style.background).toBe("rgb(211, 60, 60)");
    expect(shown).toBe("#d33c3c");

    fireEvent.click(screen.getByRole("button", { name: "Reset to default" }));
    await waitFor(() => expect(updateSettings).toHaveBeenCalledWith([{ key: "themeColor", value: null }]));
    expect(shown).toBe(DEFAULT_THEME_COLOR);
    expect(screen.queryByRole("button", { name: "Reset to default" })).toBeNull();
  });

  it("opens the wheel over the main page, shows a picked colour before Apply, and Cancel puts it back", async () => {
    setThemeColor("#d33c3c");
    renderApp(THEME_COLOR_SETTINGS_PAGE);
    fireEvent.click(screen.getByRole("button", { name: "Change theme color" }));
    expect(path).toBe(THEME_COLOR_PICKER_PAGE);
    expect(screen.getByText("main page")).toBeTruthy();
    const wheel = screen.getByRole("slider", { name: "Color wheel" });
    expect(shown).toBe("#d33c3c");

    // Round the wheel toward yellow: the launcher takes it at once, the setting stays.
    for (let i = 0; i < 12; i++) {
      fireEvent.keyDown(wheel, { key: "ArrowLeft" });
    }
    await waitFor(() => expect(shown).not.toBe("#d33c3c"));
    expect(useSettingsStore.getState().settings.themeColor).toBe("#d33c3c");

    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    expect(path).toBe(THEME_COLOR_SETTINGS_PAGE);
    expect(shown).toBe("#d33c3c");
    expect(updateSettings).not.toHaveBeenCalled();
    expect(screen.queryByRole("slider", { name: "Color wheel" })).toBeNull();
  });

  it("saves the picked colour on Apply", async () => {
    renderApp(THEME_COLOR_SETTINGS_PAGE);
    fireEvent.click(screen.getByRole("button", { name: "Change theme color" }));
    const wheel = screen.getByRole("slider", { name: "Color wheel" });
    for (let i = 0; i < 10; i++) {
      fireEvent.keyDown(wheel, { key: "ArrowUp" });
    }
    await waitFor(() => expect(shown).not.toBe(DEFAULT_THEME_COLOR));
    const picked = shown;

    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => expect(path).toBe(THEME_COLOR_SETTINGS_PAGE));
    expect(updateSettings).toHaveBeenCalledWith([{ key: "themeColor", value: picked }]);
    expect(useSettingsStore.getState().settings.themeColor).toBe(picked);
    expect(shown).toBe(picked);
    expect(screen.getByRole("button", { name: "Reset to default" })).toBeTruthy();
  });

  it("has a reset icon by Apply only while a colour is chosen, which saves the default and closes", async () => {
    renderApp("/main/replays/list");
    fireEvent.click(screen.getByRole("button", { name: "Theme Color" }));
    expect(screen.queryByRole("button", { name: "Reset to default" })).toBeNull();
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));

    setThemeColor("#d33c3c");
    fireEvent.click(screen.getByRole("button", { name: "Theme Color" }));
    fireEvent.keyDown(screen.getByRole("slider", { name: "Color wheel" }), { key: "ArrowLeft" });
    await waitFor(() => expect(shown).not.toBe("#d33c3c"));
    const reset = screen.getByRole("button", { name: "Reset to default" });
    fireEvent.mouseOver(reset);
    expect((await screen.findByRole("tooltip")).textContent).toBe("Reset to default");

    fireEvent.click(reset);
    await waitFor(() => expect(useThemeColorPickerOpen.getState().open).toBe(false));
    expect(updateSettings).toHaveBeenCalledWith([{ key: "themeColor", value: null }]);
    expect(useSettingsStore.getState().settings.themeColor).toBeNull();
    expect(shown).toBe(DEFAULT_THEME_COLOR);
    expect(path).toBe("/main/replays/list");
  });

  it("closes without saving on Escape or when the main page is left", async () => {
    renderApp(THEME_COLOR_SETTINGS_PAGE);
    fireEvent.click(screen.getByRole("button", { name: "Change theme color" }));
    fireEvent.keyDown(screen.getByRole("slider", { name: "Color wheel" }), { key: "ArrowUp" });
    await waitFor(() => expect(shown).not.toBe(DEFAULT_THEME_COLOR));
    fireEvent.keyDown(window, { key: "Escape" });
    expect(path).toBe(THEME_COLOR_SETTINGS_PAGE);
    expect(shown).toBe(DEFAULT_THEME_COLOR);

    fireEvent.click(screen.getByRole("button", { name: "Change theme color" }));
    fireEvent.keyDown(screen.getByRole("slider", { name: "Color wheel" }), { key: "ArrowUp" });
    await waitFor(() => expect(shown).not.toBe(DEFAULT_THEME_COLOR));
    act(() => navigateTo("/settings/advanced-settings"));
    expect(useThemeColorPickerOpen.getState().open).toBe(false);
    expect(shown).toBe(DEFAULT_THEME_COLOR);
    expect(updateSettings).not.toHaveBeenCalled();
  });

  it("opens from the header's palette over the page it is on, and closing it stays there", async () => {
    renderApp("/main/replays/list");
    fireEvent.click(screen.getByRole("button", { name: "Theme Color" }));
    const wheel = screen.getByRole("slider", { name: "Color wheel" });
    expect(path).toBe("/main/replays/list");

    fireEvent.keyDown(wheel, { key: "ArrowUp" });
    await waitFor(() => expect(shown).not.toBe(DEFAULT_THEME_COLOR));
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    expect(path).toBe("/main/replays/list");
    expect(shown).toBe(DEFAULT_THEME_COLOR);
    expect(screen.queryByRole("slider", { name: "Color wheel" })).toBeNull();

    fireEvent.click(screen.getByRole("button", { name: "Theme Color" }));
    fireEvent.keyDown(screen.getByRole("slider", { name: "Color wheel" }), { key: "ArrowUp" });
    await waitFor(() => expect(shown).not.toBe(DEFAULT_THEME_COLOR));
    const picked = shown;
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => expect(useThemeColorPickerOpen.getState().open).toBe(false));
    expect(path).toBe("/main/replays/list");
    expect(updateSettings).toHaveBeenCalledWith([{ key: "themeColor", value: picked }]);

    fireEvent.click(screen.getByRole("button", { name: "Theme Color" }));
    fireEvent.keyDown(window, { key: "Escape" });
    expect(path).toBe("/main/replays/list");
    expect(useThemeColorPickerOpen.getState().open).toBe(false);
  });
});
