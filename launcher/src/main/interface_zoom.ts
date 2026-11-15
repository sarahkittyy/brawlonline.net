import { DEFAULT_INTERFACE_ZOOM, INTERFACE_ZOOM_LEVELS } from "@common/interface_zoom";
import type { SettingsManager } from "@settings/settings_manager";
import type { BrowserWindow, Input, Rectangle, Screen, Size } from "electron";

/**
 * The window's smallest size at 100 % (docs/design/launcher-redesign.md checks names and rows down to a 980 px window).
 * At other sizes the page inside the frame scales with the zoom, so the layout always gets the same CSS px.
 */
export const MIN_WINDOW: Size = { width: 980, height: 450 };

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

/** The biggest level (at least 100 %) whose smallest window fits in the screen's work area. */
export function largestFittingZoom(workArea: Size, frame: Size): number {
  let largest: number = DEFAULT_INTERFACE_ZOOM;
  for (const level of INTERFACE_ZOOM_LEVELS) {
    const min = minimumWindowSize(level, frame);
    if (level > largest && min.width <= workArea.width && min.height <= workArea.height) {
      largest = level;
    }
  }
  return largest;
}

/** The window's smallest size at `zoom`: `MIN_WINDOW`'s page (inside the frame) scaled by the zoom, plus the frame. */
export function minimumWindowSize(zoom: number, frame: Size): Size {
  const scale = (min: number, frameSize: number) => Math.ceil(((min - frameSize) * zoom) / 100) + frameSize;
  return { width: scale(MIN_WINDOW.width, frame.width), height: scale(MIN_WINDOW.height, frame.height) };
}

/** The page's size in CSS px in a window of `bounds` at `zoom`. */
export function pageSize(bounds: Size, frame: Size, zoom: number): Size {
  return {
    width: ((bounds.width - frame.width) * 100) / zoom,
    height: ((bounds.height - frame.height) * 100) / zoom,
  };
}

/**
 * The window's bounds for a page of `page` CSS px at `zoom`, no smaller than `min`: centred where the window is now
 * (`bounds`), and as big as the screen's work area at most, inside it.
 */
export function windowBoundsFor(
  bounds: Rectangle,
  frame: Size,
  page: Size,
  zoom: number,
  min: Size,
  workArea: Rectangle,
): Rectangle {
  const size = (pageSide: number, frameSide: number, minSide: number, room: number) =>
    Math.min(Math.max(Math.round((pageSide * zoom) / 100) + frameSide, minSide), room);
  const width = size(page.width, frame.width, min.width, workArea.width);
  const height = size(page.height, frame.height, min.height, workArea.height);
  const place = (start: number, outer: number, side: number, areaStart: number, areaSide: number) =>
    Math.min(Math.max(Math.round(start + (outer - side) / 2), areaStart), areaStart + areaSide - side);
  return {
    x: place(bounds.x, bounds.width, width, workArea.x, workArea.width),
    y: place(bounds.y, bounds.height, height, workArea.y, workArea.height),
    width,
    height,
  };
}

const sameBounds = (a: Rectangle, b: Rectangle) =>
  a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;

type InterfaceZoomOptions = {
  isMac: boolean;
  screen: Screen;
  /** Whether the window has a minimum size (not in development). */
  enforceMinimum: boolean;
};

/**
 * Sizes the launcher by the `interfaceZoom` setting, now and whenever it changes: the page, and the window with it, so
 * a bigger size makes a bigger window rather than less room for the page. The window's minimum grows the same way, so
 * the page always has at least `MIN_WINDOW`'s; a level whose minimum window doesn't fit on the screen isn't used (the
 * biggest that fits is, and a change to it in Settings or with the keys goes back to that one). The keys above and
 * Ctrl + mouse wheel change the setting (so the size is kept for the next start). Dolphin is a separate program and is
 * not affected. The window is created at its 100 % size and hidden; this sizes it before it shows.
 */
export function attachInterfaceZoom(
  window: BrowserWindow,
  settingsManager: SettingsManager,
  { isMac, screen, enforceMinimum }: InterfaceZoomOptions,
): void {
  const { webContents } = window;
  const frameOf = (): Size => {
    const [width, height] = window.getSize();
    const [contentWidth, contentHeight] = window.getContentSize();
    return { width: width - contentWidth, height: height - contentHeight };
  };
  const workAreaOf = () => screen.getDisplayMatching(window.getBounds()).workArea;
  const fitting = () => largestFittingZoom(workAreaOf(), frameOf());
  const current = () => snapInterfaceZoom(settingsManager.get().settings.interfaceZoom);
  const used = () => Math.min(current(), fitting());

  // The zoom the window is sized for (it was created at 100 %).
  let sizedFor = DEFAULT_INTERFACE_ZOOM;
  // The bounds this last gave the window and the page size they were for: while the window still has them, the next
  // size starts from that page, not from a window the screen made smaller (200 % and back gives the 100 % window again).
  let sized: { bounds: Rectangle; page: Size } | null = null;
  const apply = () => {
    if (window.isDestroyed()) {
      return;
    }
    const zoom = used();
    if (zoom !== sizedFor) {
      const frame = frameOf();
      const min = minimumWindowSize(zoom, frame);
      if (enforceMinimum) {
        window.setMinimumSize(min.width, min.height);
      }
      if (!window.isMaximized() && !window.isFullScreen()) {
        const bounds = window.getBounds();
        const page = sized && sameBounds(sized.bounds, bounds) ? sized.page : pageSize(bounds, frame, sizedFor);
        const minForBounds = enforceMinimum ? min : { width: 0, height: 0 };
        window.setBounds(windowBoundsFor(bounds, frame, page, zoom, minForBounds, workAreaOf()));
        sized = { bounds: window.getBounds(), page };
      }
      sizedFor = zoom;
    }
    webContents.setZoomFactor(zoom / 100);
  };
  if (enforceMinimum) {
    const min = minimumWindowSize(DEFAULT_INTERFACE_ZOOM, frameOf());
    window.setMinimumSize(min.width, min.height);
  }
  apply();

  const update = (zoom: number) => {
    void settingsManager.updateSetting("interfaceZoom", Math.min(zoom, fitting()));
  };

  // Chromium keeps the zoom of a file:// page by its whole URL, the route after # included: after the player goes to
  // another page, the next resize of the window looked up the new URL, found no zoom and went back to 100 %. So the
  // zoom is set again on every load and every move between pages.
  webContents.on("did-finish-load", apply);
  webContents.on("did-navigate-in-page", apply);
  // Moved to another screen: a smaller one may not fit this size.
  window.on("moved", apply);
  const unsubscribe = settingsManager.onSettingChange("interfaceZoom", () => {
    // Set in Settings to a level too big for the screen: the setting goes back to the biggest that fits.
    if (current() > fitting()) {
      update(current());
    } else {
      apply();
    }
  });
  window.on("closed", unsubscribe);

  webContents.on("before-input-event", (event, input) => {
    const key = interfaceZoomKey(input, isMac);
    if (key) {
      event.preventDefault();
      update(key === "reset" ? DEFAULT_INTERFACE_ZOOM : stepInterfaceZoom(used(), key === "in" ? 1 : -1));
    }
  });
  webContents.on("zoom-changed", (_event, direction) => {
    update(stepInterfaceZoom(used(), direction === "in" ? 1 : -1));
  });
}
