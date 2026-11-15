import type { Input } from "electron";
import { describe, expect, it } from "vitest";

import {
  interfaceZoomKey,
  largestFittingZoom,
  minimumWindowSize,
  pageSize,
  snapInterfaceZoom,
  stepInterfaceZoom,
  windowBoundsFor,
} from "./interface_zoom";

const key = (k: string, mods: Partial<Input> = {}): Input =>
  ({ type: "keyDown", key: k, control: false, meta: false, alt: false, shift: false, ...mods } as Input);

describe("interface zoom", () => {
  it("snaps a stored value to the nearest level, or 100 % for junk", () => {
    expect(snapInterfaceZoom(100)).toBe(100);
    expect(snapInterfaceZoom(130)).toBe(125);
    expect(snapInterfaceZoom(500)).toBe(200);
    expect(snapInterfaceZoom(10)).toBe(80);
    expect(snapInterfaceZoom(undefined)).toBe(100);
    expect(snapInterfaceZoom("150")).toBe(100);
    expect(snapInterfaceZoom(NaN)).toBe(100);
  });

  it("steps one level and stops at the ends", () => {
    expect(stepInterfaceZoom(100, 1)).toBe(110);
    expect(stepInterfaceZoom(110, 1)).toBe(125);
    expect(stepInterfaceZoom(100, -1)).toBe(90);
    expect(stepInterfaceZoom(200, 1)).toBe(200);
    expect(stepInterfaceZoom(80, -1)).toBe(80);
    expect(stepInterfaceZoom(undefined, 1)).toBe(110);
  });

  it("reads Ctrl (Cmd on macOS) with + = - _ 0", () => {
    expect(interfaceZoomKey(key("=", { control: true }), false)).toBe("in");
    expect(interfaceZoomKey(key("+", { control: true, shift: true }), false)).toBe("in");
    expect(interfaceZoomKey(key("-", { control: true }), false)).toBe("out");
    expect(interfaceZoomKey(key("0", { control: true }), false)).toBe("reset");
    expect(interfaceZoomKey(key("=", { meta: true }), true)).toBe("in");
  });

  it("ignores everything else", () => {
    expect(interfaceZoomKey(key("="), false)).toBeNull();
    expect(interfaceZoomKey(key("=", { meta: true }), false)).toBeNull();
    expect(interfaceZoomKey(key("=", { control: true }), true)).toBeNull();
    expect(interfaceZoomKey(key("=", { control: true, alt: true }), false)).toBeNull();
    expect(interfaceZoomKey(key("0", { control: true, shift: true }), false)).toBeNull();
    expect(interfaceZoomKey(key("a", { control: true }), false)).toBeNull();
    expect(interfaceZoomKey({ ...key("=", { control: true }), type: "keyUp" }, false)).toBeNull();
  });

  // Windows 11's frame around the page: 16 px wide, 39 px high (title bar included).
  const frame = { width: 16, height: 39 };

  it("scales the window's minimum page with the zoom, the frame not", () => {
    expect(minimumWindowSize(100, frame)).toEqual({ width: 980, height: 450 });
    expect(minimumWindowSize(125, frame)).toEqual({ width: 1221, height: 553 });
    expect(minimumWindowSize(200, frame)).toEqual({ width: 1944, height: 861 });
  });

  it("uses the biggest level whose minimum window fits the screen, never less than 100 %", () => {
    // 1920 x 1080 at 100 % Windows scaling (taskbar 40 px).
    expect(largestFittingZoom({ width: 1920, height: 1040 }, frame)).toBe(175);
    // 1920 x 1080 at 150 %, and 1366 x 768 at 100 %.
    expect(largestFittingZoom({ width: 1280, height: 693 }, frame)).toBe(125);
    expect(largestFittingZoom({ width: 1366, height: 728 }, frame)).toBe(125);
    // 2560 x 1440: every level.
    expect(largestFittingZoom({ width: 2560, height: 1400 }, frame)).toBe(200);
    // Smaller than the 100 % window: 100 % all the same.
    expect(largestFittingZoom({ width: 800, height: 600 }, frame)).toBe(100);
  });

  it("sizes the window for the page at the zoom, centred where it was, inside the screen", () => {
    const workArea = { x: 0, y: 0, width: 1920, height: 1040 };
    const window = { x: 320, y: 145, width: 1280, height: 750 };
    const page = pageSize(window, frame, 100);
    expect(page).toEqual({ width: 1264, height: 711 });
    // The page 1264 x 711 at 125 % is 1580 x 889: the window grows around its centre.
    const at125 = windowBoundsFor(window, frame, page, 125, minimumWindowSize(125, frame), workArea);
    expect(at125).toEqual({ x: 162, y: 56, width: 1596, height: 928 });
    expect(pageSize(at125, frame, 125).width).toBe(1264);
    expect(pageSize(at125, frame, 125).height).toBeCloseTo(711, 0);
    // And back.
    expect(windowBoundsFor(at125, frame, page, 100, minimumWindowSize(100, frame), workArea)).toEqual(window);
    // Bigger than the screen: as big as the work area, inside it.
    expect(windowBoundsFor(window, frame, page, 175, minimumWindowSize(175, frame), workArea)).toEqual({
      x: 0,
      y: 0,
      width: 1920,
      height: 1040,
    });
    // In a corner of a second screen: moved back inside it.
    const second = { x: 1920, y: 0, width: 1920, height: 1040 };
    const corner = { x: 2600, y: 250, width: 1280, height: 750 };
    expect(windowBoundsFor(corner, frame, page, 125, { width: 0, height: 0 }, second)).toEqual({
      x: 2244,
      y: 112,
      width: 1596,
      height: 928,
    });
  });

  it("never makes the window smaller than its minimum", () => {
    const workArea = { x: 0, y: 0, width: 1920, height: 1040 };
    const min = minimumWindowSize(80, frame);
    const window = { x: 0, y: 0, width: 980, height: 450 };
    const small = windowBoundsFor(window, frame, pageSize(window, frame, 100), 80, min, workArea);
    expect([small.width, small.height]).toEqual([min.width, min.height]);
  });
});
