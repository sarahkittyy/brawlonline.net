import { describe, expect, it } from "vitest";

import { chooseGtkInputMethod, isSteamOs } from "./linux_input_method";

const STEAMOS = 'NAME="SteamOS"\nPRETTY_NAME="SteamOS"\nID=steamos\nID_LIKE=arch\n';
const UBUNTU = 'NAME="Ubuntu"\nID=ubuntu\nID_LIKE=debian\n';

describe("isSteamOs", () => {
  it("reads ID from os-release", () => {
    expect(isSteamOs(STEAMOS)).toBe(true);
    expect(isSteamOs('ID="steamos"\n')).toBe(true);
    expect(isSteamOs(UBUNTU)).toBe(false);
    expect(isSteamOs("ID_LIKE=steamos\n")).toBe(false);
    expect(isSteamOs(null)).toBe(false);
  });
});

describe("chooseGtkInputMethod", () => {
  it("uses the simple module on SteamOS, in desktop mode too", () => {
    expect(chooseGtkInputMethod("linux", { GTK_IM_MODULE: "ibus" }, STEAMOS)).toEqual({
      kind: "set",
      module: "simple",
      previous: "ibus",
    });
    expect(chooseGtkInputMethod("linux", {}, STEAMOS)).toEqual({ kind: "set", module: "simple", previous: undefined });
  });

  it("uses the simple module in any gamescope session", () => {
    expect(chooseGtkInputMethod("linux", { XDG_CURRENT_DESKTOP: "gamescope" }, UBUNTU).kind).toBe("set");
    expect(chooseGtkInputMethod("linux", { GAMESCOPE_WAYLAND_DISPLAY: "gamescope-0" }, null).kind).toBe("set");
  });

  it("leaves other Linux desktops and other platforms alone", () => {
    expect(chooseGtkInputMethod("linux", { GTK_IM_MODULE: "ibus", XDG_CURRENT_DESKTOP: "KDE" }, UBUNTU)).toEqual({
      kind: "unchanged",
    });
    expect(chooseGtkInputMethod("win32", { XDG_CURRENT_DESKTOP: "gamescope" }, STEAMOS)).toEqual({ kind: "unchanged" });
  });

  it("does nothing when GTK_IM_MODULE is already simple", () => {
    expect(chooseGtkInputMethod("linux", { GTK_IM_MODULE: "simple" }, STEAMOS)).toEqual({ kind: "unchanged" });
  });

  it("follows PPO_GTK_IM_MODULE", () => {
    expect(chooseGtkInputMethod("linux", { PPO_GTK_IM_MODULE: "ibus", GTK_IM_MODULE: "xim" }, STEAMOS)).toEqual({
      kind: "set",
      module: "ibus",
      previous: "xim",
    });
    expect(chooseGtkInputMethod("linux", { PPO_GTK_IM_MODULE: "keep", GTK_IM_MODULE: "ibus" }, STEAMOS)).toEqual({
      kind: "unchanged",
    });
  });
});
