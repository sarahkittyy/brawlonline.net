import { DEFAULT_INTERFACE_ZOOM, INTERFACE_ZOOM_LEVELS } from "@common/interface_zoom";
import type { SettingsManager } from "@settings/settings_manager";
import type { BrowserWindow, Input } from "electron";

/** A stored zoom as one of the levels: the nearest one, or 100 % for a value that is not a number. */
export function snapInterfaceZoom(zoom: unknown): number {
  if (typeof zoom !== "number" || !Number.isFinite(zoom)) {
    return DEFAULT_INTERFACE_ZOOM;
  }
  let nearest: number = INTERFACE_ZOOM_LEVELS[0];
  for (const level of INTERFACE_ZOOM_LEVELS) {
    if (Math.abs(level - zoom) < Math.abs(nearest - zoom)) {
      nearest = level;
    }
  }
  return nearest;
}

/** The level one step bigger (+1) or smaller (-1) than `zoom`, kept at the first and last levels. */
export function stepInterfaceZoom(zoom: unknown, direction: 1 | -1): number {
  const index = INTERFACE_ZOOM_LEVELS.indexOf(snapInterfaceZoom(zoom) as (typeof INTERFACE_ZOOM_LEVELS)[number]);
  const next = Math.min(Math.max(index + direction, 0), INTERFACE_ZOOM_LEVELS.length - 1);
  return INTERFACE_ZOOM_LEVELS[next];
}

/** What a key press does to the zoom: Ctrl (Cmd on macOS) with + or = bigger, - smaller, 0 back to 100 %. */
export function interfaceZoomKey(input: Input, isMac: boolean): "in" | "out" | "reset" | null {
  if (input.type !== "keyDown" || input.alt || !(isMac ? input.meta : input.control)) {
    return null;
  }
  switch (input.key) {
    case "+":
    case "=":
      return "in";
    case "-":
    case "_":
      return "out";
    case "0":
      return input.shift ? null : "reset";
    default:
      return null;
  }
}

/**
 * Sizes the launcher's page by the `interfaceZoom` setting, now and whenever it changes, and lets
 * the keys above and Ctrl + mouse wheel change the setting (so the size is kept for the next start).
 * Dolphin is a separate program and is not affected.
 */
export function attachInterfaceZoom(window: BrowserWindow, settingsManager: SettingsManager, isMac: boolean): void {
  const { webContents } = window;
  const apply = () => {
    if (!webContents.isDestroyed()) {
      webContents.setZoomFactor(snapInterfaceZoom(settingsManager.get().settings.interfaceZoom) / 100);
    }
  };
  const update = (zoom: number) => {
    void settingsManager.updateSetting("interfaceZoom", zoom);
  };
  const current = () => settingsManager.get().settings.interfaceZoom;

  // Chromium keeps a zoom per page origin and resets it on navigation, so set it on every load.
  webContents.on("did-finish-load", apply);
  const unsubscribe = settingsManager.onSettingChange("interfaceZoom", apply);
  window.on("closed", unsubscribe);

  webContents.on("before-input-event", (event, input) => {
    const key = interfaceZoomKey(input, isMac);
    if (key) {
      event.preventDefault();
      update(key === "reset" ? DEFAULT_INTERFACE_ZOOM : stepInterfaceZoom(current(), key === "in" ? 1 : -1));
    }
  });
  webContents.on("zoom-changed", (_event, direction) => {
    update(stepInterfaceZoom(current(), direction === "in" ? 1 : -1));
  });
}
