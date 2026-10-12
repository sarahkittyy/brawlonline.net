import type { Input } from "electron";
import { describe, expect, it } from "vitest";

import { interfaceZoomKey, snapInterfaceZoom, stepInterfaceZoom } from "./interface_zoom";

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
});
