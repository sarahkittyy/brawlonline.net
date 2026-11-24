/**
 * The input display (Settings > Game > Input Display; card 775): the player's controller drawn
 * over the game with what it holds, as streamers show their inputs. Off by default.
 *
 * Dolphin.ini `[Online] InputDisplay`, `InputDisplayX`, `InputDisplayY`, `InputDisplayWidth`: the
 * box's top-left corner and width in per mille of the game's picture; its height is half its
 * width. Online games show the player's own controller; replays and spectated games every
 * player's. (Dolphin.ini's `InputDisplayEveryone`, not set by the launcher, shows every player's
 * online too: the harness's.) The game draws it with core/ui InputDisplayUI.cpp, whose layouts the preview below draws
 * too (the same 200 x 100 canvas and numbers): change both together.
 */

export type InputDisplaySettings = {
  enabled: boolean;
  /** The box's left edge, per mille of the game's width (0-1000). */
  x: number;
  /** The box's top edge, per mille of the game's height. */
  y: number;
  /** The box's width, per mille of the game's width. */
  width: number;
};

/** core/ui's defaults (OnlineSettings.cpp): bottom left, a little over a fifth of the width. */
export const DEFAULT_INPUT_DISPLAY: InputDisplaySettings = {
  enabled: false,
  x: 20,
  y: 760,
  width: 220,
};
export const INPUT_DISPLAY_MIN_WIDTH = 60;
export const INPUT_DISPLAY_MAX_WIDTH = 600;

/** The box's height over its width (in pixels). */
export const INPUT_DISPLAY_ASPECT = 0.5;

/**
 * A setting kept inside the picture as the game draws it: whole numbers, the width in its range,
 * the box not past the picture's right or bottom edge (`pictureAspect` = width / height).
 */
export function normalizeInputDisplay(s: Partial<InputDisplaySettings> | null | undefined, pictureAspect = 16 / 9) {
  const d = DEFAULT_INPUT_DISPLAY;
  const num = (v: unknown, def: number) => (typeof v === "number" && Number.isFinite(v) ? Math.round(v) : def);
  const width = Math.min(INPUT_DISPLAY_MAX_WIDTH, Math.max(INPUT_DISPLAY_MIN_WIDTH, num(s?.width, d.width)));
  // The box's height in per mille of the picture's height.
  const height = Math.round(width * INPUT_DISPLAY_ASPECT * pictureAspect);
  const x = Math.min(1000 - width, Math.max(0, num(s?.x, d.x)));
  const y = Math.min(Math.max(0, 1000 - height), Math.max(0, num(s?.y, d.y)));
  const bool = (v: unknown, def: boolean) => (typeof v === "boolean" ? v : def);
  return { enabled: bool(s?.enabled, d.enabled), x, y, width };
}

/** The controllers the display draws (gfPadType). */
export type InputDisplayController = "gc" | "classic" | "wiimote" | "nunchuk";

/** One part of a controller's drawing on the 200 x 100 canvas (InputDisplayUI.cpp's Element). */
export type InputDisplayElement =
  | { kind: "body"; x0: number; y0: number; x1: number; y1: number }
  | { kind: "circle"; cx: number; cy: number; r: number; label: string; color: string }
  | { kind: "pill"; x0: number; y0: number; x1: number; y1: number; label: string; color: string }
  | { kind: "dpad"; cx: number; cy: number; arm: number; thick: number }
  | { kind: "stick"; cx: number; cy: number; r: number; color: string; octagon?: boolean }
  | { kind: "trigger"; x0: number; y0: number; x1: number; y1: number; label: string };

const GREEN = "rgb(40, 200, 120)";
const RED = "rgb(232, 72, 72)";
const LIGHT = "rgb(222, 222, 222)";
const PURPLE = "rgb(150, 110, 235)";
const YELLOW = "rgb(246, 200, 40)";
const BLUE = "rgb(110, 180, 255)";

export const INPUT_DISPLAY_LAYOUTS: Record<InputDisplayController, readonly InputDisplayElement[]> = {
  gc: [
    { kind: "trigger", x0: 12, y0: 8, x1: 62, y1: 17, label: "L" },
    { kind: "trigger", x0: 138, y0: 8, x1: 188, y1: 17, label: "R" },
    { kind: "pill", x0: 150, y0: 21, x1: 178, y1: 28, label: "Z", color: PURPLE },
    { kind: "stick", cx: 44, cy: 56, r: 23, color: LIGHT, octagon: true },
    { kind: "dpad", cx: 80, cy: 82, arm: 8, thick: 6 },
    { kind: "circle", cx: 100, cy: 46, r: 4.5, label: "", color: LIGHT },
    { kind: "stick", cx: 116, cy: 82, r: 13, color: YELLOW, octagon: true },
    { kind: "circle", cx: 160, cy: 58, r: 11, label: "A", color: GREEN },
    { kind: "circle", cx: 140, cy: 75, r: 7, label: "B", color: RED },
    { kind: "circle", cx: 182, cy: 52, r: 7, label: "X", color: LIGHT },
    { kind: "circle", cx: 158, cy: 36, r: 7, label: "Y", color: LIGHT },
  ],
  classic: [
    { kind: "trigger", x0: 12, y0: 8, x1: 62, y1: 17, label: "L" },
    { kind: "trigger", x0: 138, y0: 8, x1: 188, y1: 17, label: "R" },
    { kind: "pill", x0: 40, y0: 21, x1: 62, y1: 28, label: "ZL", color: LIGHT },
    { kind: "pill", x0: 138, y0: 21, x1: 160, y1: 28, label: "ZR", color: LIGHT },
    { kind: "dpad", cx: 36, cy: 50, arm: 9, thick: 7 },
    { kind: "circle", cx: 86, cy: 50, r: 4.5, label: "-", color: LIGHT },
    { kind: "circle", cx: 114, cy: 50, r: 4.5, label: "+", color: LIGHT },
    { kind: "circle", cx: 164, cy: 38, r: 7, label: "x", color: LIGHT },
    { kind: "circle", cx: 177, cy: 51, r: 7, label: "a", color: LIGHT },
    { kind: "circle", cx: 164, cy: 64, r: 7, label: "b", color: LIGHT },
    { kind: "circle", cx: 151, cy: 51, r: 7, label: "y", color: LIGHT },
    { kind: "stick", cx: 76, cy: 78, r: 15, color: LIGHT },
    { kind: "stick", cx: 124, cy: 78, r: 15, color: LIGHT },
  ],
  wiimote: [
    { kind: "body", x0: 10, y0: 30, x1: 190, y1: 74 },
    { kind: "dpad", cx: 36, cy: 52, arm: 10, thick: 7 },
    { kind: "circle", cx: 68, cy: 52, r: 8, label: "A", color: BLUE },
    { kind: "circle", cx: 94, cy: 52, r: 4.5, label: "-", color: LIGHT },
    { kind: "circle", cx: 114, cy: 52, r: 4.5, label: "+", color: LIGHT },
    { kind: "circle", cx: 146, cy: 52, r: 7.5, label: "1", color: LIGHT },
    { kind: "circle", cx: 170, cy: 52, r: 7.5, label: "2", color: LIGHT },
    { kind: "pill", x0: 54, y0: 80, x1: 82, y1: 90, label: "B", color: BLUE },
  ],
  nunchuk: [
    { kind: "body", x0: 8, y0: 14, x1: 96, y1: 90 },
    { kind: "stick", cx: 44, cy: 56, r: 22, color: LIGHT },
    { kind: "circle", cx: 80, cy: 31, r: 6, label: "C", color: LIGHT },
    { kind: "pill", x0: 71, y0: 43, x1: 89, y1: 58, label: "Z", color: LIGHT },
    { kind: "body", x0: 116, y0: 6, x1: 176, y1: 94 },
    { kind: "dpad", cx: 146, cy: 22, arm: 8, thick: 6 },
    { kind: "circle", cx: 146, cy: 44, r: 7, label: "A", color: BLUE },
    { kind: "circle", cx: 132, cy: 62, r: 4, label: "-", color: LIGHT },
    { kind: "circle", cx: 160, cy: 62, r: 4, label: "+", color: LIGHT },
    { kind: "circle", cx: 146, cy: 74, r: 5.5, label: "1", color: LIGHT },
    { kind: "circle", cx: 146, cy: 87, r: 5.5, label: "2", color: LIGHT },
    { kind: "pill", x0: 181, y0: 36, x1: 192, y1: 56, label: "B", color: BLUE },
  ],
};

/** `input-display.json` (core: Online/InputView.h PollLauncherFile): the setting, for a running game. */
export const INPUT_DISPLAY_REQUEST_FILE = "input-display.json";
