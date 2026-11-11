import BrightnessHighRoundedIcon from "@mui/icons-material/BrightnessHighRounded";
import BrightnessLowRoundedIcon from "@mui/icons-material/BrightnessLowRounded";
import Button from "@mui/material/Button";
import Slider from "@mui/material/Slider";
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
  oklchToRgb,
  THEME_LIGHTNESS,
  wheelPositionOf,
} from "@/styles/theme_color";

import { ThemeColorPickerMessages as Messages } from "./theme_color_picker.messages";
import styles from "./theme_color_picker.module.css";
import {
  closeThemeColorPicker,
  saveThemeColor,
  THEME_COLOR_SETTINGS_PAGE,
  useThemeColorPickerOpen,
} from "./use_theme_color_picker";

/** The wheel's size in CSS pixels; the canvas is drawn at this size and its round edge is CSS's (crisp at any zoom). */
const WHEEL_SIZE = 300;
const HUE_STEP = 5;
const RADIUS_STEP = 0.05;

/**
 * Draws the wheel at a lightness: hue around the circle (OKLCH hue, counter-clockwise from the right), the
 * hue's most sRGB chroma at the rim and grey at the centre, so every point is a colour the theme can use.
 */
function drawWheel(canvas: HTMLCanvasElement, l: number) {
  const ctx = canvas.getContext("2d");
  if (!ctx) {
    return;
  }
  const size = canvas.width;
  const max = Array.from({ length: 360 }, (_, h) => maxChroma(l, h));
  const image = ctx.createImageData(size, size);
  const px = image.data;
  const half = size / 2;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const dx = x + 0.5 - half;
      const dy = half - (y + 0.5);
      const r = Math.min(1, Math.hypot(dx, dy) / half);
      const h = ((Math.atan2(dy, dx) * 180) / Math.PI + 360) % 360;
      const [cr, cg, cb] = oklchToRgb({ l, c: r * max[Math.floor(h) % 360], h });
      const i = (y * size + x) * 4;
      px[i] = cr;
      px[i + 1] = cg;
      px[i + 2] = cb;
      px[i + 3] = 255;
    }
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

  React.useEffect(() => {
    if (canvasRef.current) {
      drawWheel(canvasRef.current, position.l);
    }
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
    closeThemeColorPicker();
    navigate(THEME_COLOR_SETTINGS_PAGE);
  }, [navigate]);

  const apply = React.useCallback(() => {
    void saveThemeColor(colorAtWheelPosition(position)).catch(log.error).finally(back);
  }, [position, back]);

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
            sx={{
              "& .MuiSlider-rail": { background: brightnessTrack, opacity: 1, height: 8 },
              "& .MuiSlider-thumb": { background: color, border: "2px solid #fff" },
            }}
          />
          <BrightnessHighRoundedIcon fontSize="small" />
        </div>
        <div className={styles.buttons}>
          <Button variant="outlined" color="inherit" onClick={back}>
            {Messages.cancel()}
          </Button>
          <Button variant="contained" color="primary" onClick={apply}>
            {Messages.apply()}
          </Button>
        </div>
      </div>
    </div>
  );
};
