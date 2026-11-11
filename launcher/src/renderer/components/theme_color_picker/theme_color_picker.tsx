import BrightnessHighRoundedIcon from "@mui/icons-material/BrightnessHighRounded";
import BrightnessLowRoundedIcon from "@mui/icons-material/BrightnessLowRounded";
import RefreshRoundedIcon from "@mui/icons-material/RefreshRounded";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import Slider from "@mui/material/Slider";
import Tooltip from "@mui/material/Tooltip";
import log from "electron-log";
import React from "react";
import { useLocation, useNavigate } from "react-router-dom";

import { useThemeColor } from "@/lib/hooks/use_settings";
import { useDefaultThemeColor, useThemeColorDraft } from "@/styles/game_theme_provider";
import type { WheelPosition } from "@/styles/theme_color";
import {
  colorAtWheelPosition,
  maxChroma,
  normalizeThemeColor,
  THEME_LIGHTNESS,
  wheelPositionOf,
} from "@/styles/theme_color";

import { ThemeColorPickerMessages as Messages } from "./theme_color_picker.messages";
import styles from "./theme_color_picker.module.css";
import { closeThemeColorPicker, saveThemeColor, useThemeColorPickerOpen } from "./use_theme_color_picker";

/** The wheel's size in CSS pixels; the canvas is drawn at this size and its round edge is CSS's (crisp at any zoom). */
const WHEEL_SIZE = 300;
const HUE_STEP = 5;
const RADIUS_STEP = 0.05;
/** Fixed, with the moving colours in variables: an sx that changes on every move adds an emotion rule each time. */
const BRIGHTNESS_SX = {
  "& .MuiSlider-rail": { background: "var(--brightness-track)", opacity: 1, height: 8 },
  "& .MuiSlider-thumb": { background: "var(--brightness-thumb)", border: "2px solid #fff" },
};

/** Each pixel's place on the wheel, worked out once (the brightness slider redraws the wheel as it moves). */
type WheelGeometry = { r: Float32Array; hue: Uint16Array; frac: Float32Array; cos: Float32Array; sin: Float32Array };
let wheelGeometry: WheelGeometry | null = null;

function geometryOf(size: number): WheelGeometry {
  const n = size * size;
  if (wheelGeometry?.r.length === n) {
    return wheelGeometry;
  }
  const g: WheelGeometry = {
    r: new Float32Array(n),
    hue: new Uint16Array(n),
    frac: new Float32Array(n),
    cos: new Float32Array(n),
    sin: new Float32Array(n),
  };
  const half = size / 2;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const p = y * size + x;
      const dx = x + 0.5 - half;
      const dy = half - (y + 0.5);
      const angle = Math.atan2(dy, dx);
      const h = ((angle * 180) / Math.PI + 360) % 360;
      g.r[p] = Math.min(1, Math.hypot(dx, dy) / half);
      g.hue[p] = Math.floor(h) % 360;
      g.frac[p] = h - Math.floor(h);
      g.cos[p] = Math.cos(angle);
      g.sin[p] = Math.sin(angle);
    }
  }
  wheelGeometry = g;
  return g;
}

/** Linear 0-1 to an 8-bit sRGB channel by table (a fifth of a level at most off the exact curve near black). */
const SRGB_STEPS = 16384;
let srgbTable: Uint8ClampedArray | null = null;

function srgbEncoder(): (v: number) => number {
  if (!srgbTable) {
    srgbTable = new Uint8ClampedArray(SRGB_STEPS + 1);
    for (let i = 0; i <= SRGB_STEPS; i++) {
      const v = i / SRGB_STEPS;
      srgbTable[i] = 255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * v ** (1 / 2.4) - 0.055);
    }
  }
  const table = srgbTable;
  return (v) => table[((v < 0 ? 0 : v > 1 ? 1 : v) * SRGB_STEPS + 0.5) | 0];
}

/**
 * Draws the wheel at a lightness: hue around the circle (OKLCH hue, counter-clockwise from the right), the
 * hue's most sRGB chroma at the rim and grey at the centre, so every point is a colour the theme can use.
 * oklchToRgb inlined over typed arrays and an sRGB table, the chroma capped at the most for the pixel's hue (interpolated between
 * whole degrees) instead of searched per pixel: about 6x faster, within a few levels of it at the rim.
 */
function drawWheel(canvas: HTMLCanvasElement, l: number) {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  const size = canvas.width;
  const g = geometryOf(size);
  const encode = srgbEncoder();
  const max = new Float32Array(361);
  for (let h = 0; h < 360; h++) {
    max[h] = maxChroma(l, h);
  }
  max[360] = max[0];
  const image = ctx.createImageData(size, size);
  const px = image.data;
  for (let p = 0; p < size * size; p++) {
    const h = g.hue[p];
    const cap = max[h] + (max[h + 1] - max[h]) * g.frac[p];
    const c = Math.min(g.r[p] * max[h], cap);
    const a = c * g.cos[p];
    const b = c * g.sin[p];
    // OKLab to linear sRGB, as theme_color.ts oklchToLinear.
    const l_ = (l + 0.3963377774 * a + 0.2158037573 * b) ** 3;
    const m_ = (l - 0.1055613458 * a - 0.0638541728 * b) ** 3;
    const s_ = (l - 0.0894841775 * a - 1.291485548 * b) ** 3;
    const i = p * 4;
    px[i] = encode(4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_);
    px[i + 1] = encode(-1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_);
    px[i + 2] = encode(-0.0041960863 * l_ - 0.7034186147 * m_ + 1.707614701 * s_);
    px[i + 3] = 255;
  }
  ctx.putImageData(image, 0, 0);
}

/** A wheel position from a pointer over the wheel element. */
function positionAt(el: HTMLElement, clientX: number, clientY: number, l: number): WheelPosition {
  const rect = el.getBoundingClientRect();
  const dx = clientX - (rect.left + rect.width / 2);
  const dy = rect.top + rect.height / 2 - clientY;
  const r = Math.min(1, Math.hypot(dx, dy) / (rect.width / 2));
  const h = ((Math.atan2(dy, dx) * 180) / Math.PI + 360) % 360;
  return { h, r, l };
}

/**
 * Settings > Appearance > Theme Color's picker: a colour wheel over the middle of the main page, which
 * takes each colour as it is picked (the setting changes only on Apply). Cancel, Escape or leaving the main
 * page (a shortcut) puts the look back. Rendered beside the routes, so it sees the page change.
 */
export const ThemeColorPicker = React.memo(() => {
  const open = useThemeColorPickerOpen((s) => s.open);
  const onMainPage = useLocation().pathname.startsWith("/main");
  const shown = React.useRef(false);
  React.useEffect(() => {
    if (!open) {
      shown.current = false;
    } else if (onMainPage) {
      shown.current = true;
    } else if (shown.current) {
      closeThemeColorPicker();
    }
  }, [open, onMainPage]);
  if (!open || !onMainPage) {
    return null;
  }
  return <ThemeColorPickerPanel />;
});

const ThemeColorPickerPanel = () => {
  const navigate = useNavigate();
  const [setting] = useThemeColor();
  const defaultColor = useDefaultThemeColor((s) => s.color);
  const draft = useThemeColorDraft((s) => s.draft);
  // The plain look's light grey is above the wheel's lightness: it starts in the grey centre at a middle one.
  const startColor = normalizeThemeColor(setting) ?? defaultColor;
  const [position, setPosition] = React.useState<WheelPosition>(() =>
    startColor ? wheelPositionOf(startColor) : { h: 0, r: 0, l: THEME_LIGHTNESS.initial },
  );
  const color = draft ?? colorAtWheelPosition(position);

  const canvasRef = React.useRef<HTMLCanvasElement>(null);
  const wheelRef = React.useRef<HTMLDivElement>(null);
  const dragging = React.useRef(false);

  // Once a frame at most: the slider can move several times a frame.
  React.useEffect(() => {
    const draw = requestAnimationFrame(() => canvasRef.current && drawWheel(canvasRef.current, position.l));
    return () => cancelAnimationFrame(draw);
  }, [position.l]);

  // The launcher re-themes at most once a frame while the wheel is dragged.
  const frame = React.useRef<number | null>(null);
  const pending = React.useRef<WheelPosition | null>(null);
  const pick = React.useCallback((next: WheelPosition) => {
    setPosition(next);
    pending.current = next;
    if (frame.current === null) {
      frame.current = requestAnimationFrame(() => {
        frame.current = null;
        if (pending.current) {
          useThemeColorDraft.setState({ draft: colorAtWheelPosition(pending.current) });
        }
      });
    }
  }, []);
  React.useEffect(
    () => () => {
      if (frame.current !== null) {
        cancelAnimationFrame(frame.current);
      }
    },
    [],
  );

  const back = React.useCallback(() => {
    const { returnTo } = useThemeColorPickerOpen.getState();
    closeThemeColorPicker();
    if (returnTo) {
      navigate(returnTo);
    }
  }, [navigate]);

  const apply = React.useCallback(() => {
    void saveThemeColor(colorAtWheelPosition(position)).catch(log.error).finally(back);
  }, [position, back]);

  // Shown while a colour is chosen, as in Settings: the default look is saved at once and the picker closes.
  const chosen = normalizeThemeColor(setting) !== null;
  const reset = React.useCallback(() => {
    void saveThemeColor(null).catch(log.error).finally(back);
  }, [back]);

  React.useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation();
        back();
      }
    };
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, [back]);

  const onPointerDown = (e: React.PointerEvent<HTMLDivElement>) => {
    dragging.current = true;
    e.currentTarget.setPointerCapture(e.pointerId);
    pick(positionAt(e.currentTarget, e.clientX, e.clientY, position.l));
  };
  const onPointerMove = (e: React.PointerEvent<HTMLDivElement>) => {
    if (dragging.current) {
      pick(positionAt(e.currentTarget, e.clientX, e.clientY, position.l));
    }
  };
  const onPointerUp = () => {
    dragging.current = false;
  };
  const onKeyDown = (e: React.KeyboardEvent) => {
    const turn = { ArrowLeft: HUE_STEP, ArrowRight: -HUE_STEP }[e.key];
    const out = { ArrowUp: RADIUS_STEP, ArrowDown: -RADIUS_STEP }[e.key];
    if (turn === undefined && out === undefined) {
      return;
    }
    e.preventDefault();
    pick({
      ...position,
      h: (position.h + (turn ?? 0) + 360) % 360,
      r: Math.max(0, Math.min(1, position.r + (out ?? 0))),
    });
  };

  const markerAngle = (position.h * Math.PI) / 180;
  const markerX = (Math.cos(markerAngle) * position.r * WHEEL_SIZE) / 2;
  const markerY = (-Math.sin(markerAngle) * position.r * WHEEL_SIZE) / 2;
  const brightnessTrack = `linear-gradient(to right, ${colorAtWheelPosition({
    ...position,
    l: THEME_LIGHTNESS.min,
  })}, ${colorAtWheelPosition({ ...position, l: THEME_LIGHTNESS.max })})`;

  return (
    <div className={styles.backdrop}>
      <div className={styles.panel} role="dialog" aria-label={Messages.themeColor()}>
        <div
          ref={wheelRef}
          className={styles.wheel}
          style={{ width: WHEEL_SIZE, height: WHEEL_SIZE }}
          role="slider"
          tabIndex={0}
          aria-label={Messages.colorWheel()}
          aria-valuetext={color}
          onPointerDown={onPointerDown}
          onPointerMove={onPointerMove}
          onPointerUp={onPointerUp}
          onPointerCancel={onPointerUp}
          onKeyDown={onKeyDown}
        >
          <canvas ref={canvasRef} width={WHEEL_SIZE} height={WHEEL_SIZE} className={styles.canvas} />
          <div
            className={styles.marker}
            style={{ background: color, transform: `translate(${markerX}px, ${markerY}px)` }}
          />
        </div>
        <div className={styles.brightness}>
          <BrightnessLowRoundedIcon fontSize="small" />
          <Slider
            aria-label={Messages.brightness()}
            value={position.l}
            min={THEME_LIGHTNESS.min}
            max={THEME_LIGHTNESS.max}
            step={0.005}
            track={false}
            onChange={(_, v) => pick({ ...position, l: v as number })}
            style={{ "--brightness-track": brightnessTrack, "--brightness-thumb": color } as React.CSSProperties}
            sx={BRIGHTNESS_SX}
          />
          <BrightnessHighRoundedIcon fontSize="small" />
        </div>
        <div className={styles.buttons}>
          <Button variant="outlined" color="inherit" onClick={back}>
            {Messages.cancel()}
          </Button>
          <div className={styles.applyGroup}>
            {chosen && (
              <Tooltip title={Messages.resetToDefault()}>
                <IconButton
                  aria-label={Messages.resetToDefault()}
                  color="inherit"
                  className={styles.reset}
                  onClick={reset}
                >
                  <RefreshRoundedIcon />
                </IconButton>
              </Tooltip>
            )}
            <Button variant="contained" color="primary" onClick={apply}>
              {Messages.apply()}
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
};
