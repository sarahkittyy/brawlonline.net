import type { InputDisplayController, InputDisplayElement, InputDisplaySettings } from "@common/input_display";
import {
  INPUT_DISPLAY_ASPECT,
  INPUT_DISPLAY_LAYOUTS,
  INPUT_DISPLAY_MAX_WIDTH,
  INPUT_DISPLAY_MIN_WIDTH,
  normalizeInputDisplay,
} from "@common/input_display";
import { css } from "@emotion/react";
import React from "react";

// The game's picture: 16:9 (the launcher plays P+ in widescreen).
const PICTURE_ASPECT = 16 / 9;

const OUTLINE = "rgba(255, 255, 255, 0.78)";
const BODY = "rgba(255, 255, 255, 0.27)";
const IDLE = "rgba(0, 0, 0, 0.31)";
const LABEL = "rgba(255, 255, 255, 0.9)";

/** One controller as the game draws it (core/ui InputDisplayUI.cpp), at rest, on its 200 x 100 canvas. */
export const InputDisplayDrawing = ({ controller }: { controller: InputDisplayController }) => (
  <svg viewBox="0 0 200 100" width="100%" height="100%" style={{ display: "block" }}>
    <rect x={0} y={0} width={200} height={100} rx={8} fill="rgba(0, 0, 0, 0.43)" />
    {INPUT_DISPLAY_LAYOUTS[controller].map((e, i) => (
      <Element key={i} e={e} />
    ))}
  </svg>
);

const Label = ({ x, y, size, text }: { x: number; y: number; size: number; text: string }) =>
  text ? (
    <text
      x={x}
      y={y}
      fontSize={size}
      fill={LABEL}
      textAnchor="middle"
      dominantBaseline="central"
      fontFamily="sans-serif"
      fontWeight={600}
    >
      {text}
    </text>
  ) : null;

const Element = ({ e }: { e: InputDisplayElement }) => {
  const line = 1.3;
  switch (e.kind) {
    case "body":
      return (
        <rect
          x={e.x0}
          y={e.y0}
          width={e.x1 - e.x0}
          height={e.y1 - e.y0}
          rx={12}
          fill="none"
          stroke={BODY}
          strokeWidth={line}
        />
      );
    case "circle":
      return (
        <g>
          <circle cx={e.cx} cy={e.cy} r={e.r} fill={IDLE} stroke={OUTLINE} strokeWidth={line} />
          <Label x={e.cx} y={e.cy} size={Math.max(e.r * 1.25, 7)} text={e.label} />
        </g>
      );
    case "pill": {
      const r = Math.min(e.x1 - e.x0, e.y1 - e.y0) / 2;
      return (
        <g>
          <rect
            x={e.x0}
            y={e.y0}
            width={e.x1 - e.x0}
            height={e.y1 - e.y0}
            rx={r}
            fill={IDLE}
            stroke={OUTLINE}
            strokeWidth={line}
          />
          <Label x={(e.x0 + e.x1) / 2} y={(e.y0 + e.y1) / 2} size={7.5} text={e.label} />
        </g>
      );
    }
    case "trigger": {
      const r = (e.y1 - e.y0) / 2;
      return (
        <g>
          <rect
            x={e.x0}
            y={e.y0}
            width={e.x1 - e.x0}
            height={e.y1 - e.y0}
            rx={r}
            fill={IDLE}
            stroke={OUTLINE}
            strokeWidth={line}
          />
          <Label x={(e.x0 + e.x1) / 2} y={(e.y0 + e.y1) / 2} size={7.5} text={e.label} />
        </g>
      );
    }
    case "dpad": {
      const { cx, cy, arm: a } = e;
      const t = e.thick / 2;
      const points = [
        [cx - t, cy - a],
        [cx + t, cy - a],
        [cx + t, cy - t],
        [cx + a, cy - t],
        [cx + a, cy + t],
        [cx + t, cy + t],
        [cx + t, cy + a],
        [cx - t, cy + a],
        [cx - t, cy + t],
        [cx - a, cy + t],
        [cx - a, cy - t],
        [cx - t, cy - t],
      ];
      return (
        <polygon points={points.map((p) => p.join(",")).join(" ")} fill={IDLE} stroke={OUTLINE} strokeWidth={line} />
      );
    }
    case "stick": {
      const dot = Math.max(e.r * 0.3, 2);
      const gate = e.octagon ? (
        <polygon
          points={Array.from({ length: 8 }, (_, i) => {
            // ImGui's AddNgon: the first corner at angle 0.
            const angle = (i / 8) * Math.PI * 2;
            return `${e.cx + Math.cos(angle) * e.r},${e.cy + Math.sin(angle) * e.r}`;
          }).join(" ")}
          fill={IDLE}
          stroke={OUTLINE}
          strokeWidth={line}
        />
      ) : (
        <circle cx={e.cx} cy={e.cy} r={e.r} fill={IDLE} stroke={OUTLINE} strokeWidth={line} />
      );
      return (
        <g>
          {gate}
          <circle cx={e.cx} cy={e.cy} r={dot} fill={e.color} />
        </g>
      );
    }
  }
};

type Drag = {
  mode: "move" | "resize";
  pointerId: number;
  startX: number;
  startY: number;
  from: InputDisplaySettings;
};

/**
 * The game's picture with the input display's box on it, for `controller`: drag the box to move it,
 * its corner to resize it (its shape stays). `onChange` once it is let go.
 */
export const InputDisplayPreview = ({
  value,
  controller,
  disabled,
  onChange,
}: {
  value: InputDisplaySettings;
  controller: InputDisplayController;
  disabled?: boolean;
  onChange: (value: InputDisplaySettings) => void;
}) => {
  const picture = React.useRef<HTMLDivElement>(null);
  const [shown, setShown] = React.useState(value);
  const drag = React.useRef<Drag | null>(null);
  React.useEffect(() => {
    if (!drag.current) {
      setShown(value);
    }
  }, [value]);

  const start = (mode: Drag["mode"]) => (ev: React.PointerEvent) => {
    if (disabled || ev.button !== 0) {
      return;
    }
    ev.preventDefault();
    ev.stopPropagation();
    (ev.currentTarget as Element).setPointerCapture(ev.pointerId);
    drag.current = { mode, pointerId: ev.pointerId, startX: ev.clientX, startY: ev.clientY, from: shown };
  };

  const move = (ev: React.PointerEvent) => {
    const d = drag.current;
    const rect = picture.current?.getBoundingClientRect();
    if (!d || d.pointerId !== ev.pointerId || !rect || rect.width === 0) {
      return;
    }
    // Per mille of the picture.
    const dx = ((ev.clientX - d.startX) / rect.width) * 1000;
    const dy = ((ev.clientY - d.startY) / rect.height) * 1000;
    if (d.mode === "move") {
      setShown(normalizeInputDisplay({ ...d.from, x: d.from.x + dx, y: d.from.y + dy }, PICTURE_ASPECT));
    } else {
      // The corner follows the pointer along the box's diagonal (its shape kept).
      const byX = d.from.width + dx;
      const byY = d.from.width + dy / (INPUT_DISPLAY_ASPECT * PICTURE_ASPECT);
      const width = Math.min(
        INPUT_DISPLAY_MAX_WIDTH,
        1000 - d.from.x,
        (1000 - d.from.y) / (INPUT_DISPLAY_ASPECT * PICTURE_ASPECT),
        Math.max(INPUT_DISPLAY_MIN_WIDTH, (byX + byY) / 2),
      );
      setShown(normalizeInputDisplay({ ...d.from, width }, PICTURE_ASPECT));
    }
  };

  const end = (ev: React.PointerEvent) => {
    const d = drag.current;
    if (!d || d.pointerId !== ev.pointerId) {
      return;
    }
    drag.current = null;
    if (shown.x !== d.from.x || shown.y !== d.from.y || shown.width !== d.from.width) {
      onChange(shown);
    }
  };

  const heightPerMille = shown.width * INPUT_DISPLAY_ASPECT * PICTURE_ASPECT;
  return (
    <div
      ref={picture}
      data-testid="input-display-preview"
      onPointerMove={move}
      onPointerUp={end}
      onPointerCancel={end}
      css={css`
        position: relative;
        width: 100%;
        max-width: 360px;
        aspect-ratio: 16 / 9;
        border-radius: 6px;
        border: 1px solid var(--line-soft);
        background: linear-gradient(
          180deg,
          rgba(110, 150, 210, 0.28) 0%,
          rgba(40, 50, 80, 0.35) 62%,
          rgba(25, 25, 30, 0.6) 100%
        );
        overflow: hidden;
        user-select: none;
        touch-action: none;
        opacity: ${disabled ? 0.5 : 1};
      `}
    >
      {/* A stage's floor, for where the box sits against the game. */}
      <div
        css={css`
          position: absolute;
          left: 18%;
          right: 18%;
          top: 66%;
          height: 3%;
          border-radius: 2px;
          background: rgba(255, 255, 255, 0.18);
        `}
      />
      <div
        data-testid="input-display-box"
        onPointerDown={start("move")}
        css={css`
          position: absolute;
          left: ${shown.x / 10}%;
          top: ${shown.y / 10}%;
          width: ${shown.width / 10}%;
          height: ${heightPerMille / 10}%;
          cursor: ${disabled ? "default" : "move"};
          outline: 1px dashed ${disabled ? "transparent" : "rgba(255, 255, 255, 0.45)"};
          outline-offset: 2px;
        `}
      >
        <InputDisplayDrawing controller={controller} />
        {!disabled && (
          <div
            data-testid="input-display-resize"
            onPointerDown={start("resize")}
            css={css`
              position: absolute;
              right: -6px;
              bottom: -6px;
              width: 12px;
              height: 12px;
              border-radius: 3px;
              background: var(--accent-primary);
              border: 1px solid rgba(255, 255, 255, 0.85);
              cursor: nwse-resize;
            `}
          />
        )}
      </div>
    </div>
  );
};
