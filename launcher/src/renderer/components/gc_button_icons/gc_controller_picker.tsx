/**
 * A GameCube controller whose buttons are clicked on and off (Settings > Clips > Manual Clip
 * Hotkey's Configure): the picked ones glow. The controller is Zacksly's "Controller Colored" (the
 * same pack as the button icons), cropped to its body; the face buttons, Start and the D-pad are
 * click areas laid over it, and L, R and Z, which a front view hardly shows, are their icons above
 * the shoulders, as the pack's own control screen has them.
 */
import { GC_BUTTONS } from "@common/clip_hotkey";
import { css } from "@emotion/react";

import controllerImage from "./images/Controller.svg";
import lIcon from "./images/L.svg";
import rIcon from "./images/R.svg";
import zIcon from "./images/Z.svg";

// Positions are in the pack's 4096x2160 drawing. The picture shows x 1100-3000, y 40-1800 of it:
// the controller (Controller.svg's view box, 1170 410 1760 1360) and the shoulder icons above.
const FRAME = { x: 1100, y: 40, w: 1900, h: 1760 };
const CONTROLLER = { x: 1170, y: 410, w: 1760, h: 1360 };

const left = (x: number) => `${((x - FRAME.x) / FRAME.w) * 100}%`;
const top = (y: number) => `${((y - FRAME.y) / FRAME.h) * 100}%`;
const width = (w: number) => `${(w / FRAME.w) * 100}%`;
const height = (h: number) => `${(h / FRAME.h) * 100}%`;

type Spot = { bit: number; x: number; y: number; w: number; h: number; rotate?: number; round?: string };

/** Click areas over the drawing's buttons: centre, size, tilt (measured from its 4k render). */
const SPOTS: Spot[] = [
  { bit: 0x0100, x: 2582, y: 830, w: 210, h: 210 }, // A
  { bit: 0x0200, x: 2382, y: 927, w: 130, h: 130 }, // B
  { bit: 0x0800, x: 2527, y: 647, w: 190, h: 120, rotate: -12 }, // Y
  { bit: 0x0400, x: 2771, y: 789, w: 120, h: 180, rotate: -15 }, // X
  { bit: 0x1000, x: 2048, y: 848, w: 120, h: 120 }, // Start
  { bit: 0x0008, x: 1762, y: 1146, w: 78, h: 78, round: "18%" }, // D-pad
  { bit: 0x0004, x: 1762, y: 1282, w: 78, h: 78, round: "18%" },
  { bit: 0x0001, x: 1694, y: 1214, w: 78, h: 78, round: "18%" },
  { bit: 0x0002, x: 1830, y: 1214, w: 78, h: 78, round: "18%" },
];

/** L, R and Z: their icons, centre and height (widths from the icons' own shapes). */
const SHOULDERS = [
  { bit: 0x0040, src: lIcon, x: 1470, y: 250, h: 170, aspect: 498 / 399 },
  { bit: 0x0020, src: rIcon, x: 2590, y: 170, h: 150, aspect: 498 / 399 },
  { bit: 0x0010, src: zIcon, x: 2590, y: 330, h: 100, aspect: 462 / 234 },
];

const GLOW = "rgba(255, 228, 92, 0.9)";
const GLOW_FAR = "rgba(255, 210, 60, 0.45)";

const nameOf = (bit: number) => GC_BUTTONS.find((b) => b.bit === bit)?.name ?? "";

const buttonReset = css`
  position: absolute;
  padding: 0;
  border: none;
  background: transparent;
  cursor: pointer;
  &:focus-visible {
    outline: 2px solid white;
    outline-offset: 2px;
  }
`;

export const GcControllerPicker = ({ buttons, onToggle }: { buttons: number; onToggle: (bit: number) => void }) => (
  <div
    css={css`
      position: relative;
      width: 100%;
      aspect-ratio: ${FRAME.w} / ${FRAME.h};
      user-select: none;
    `}
  >
    <img
      src={controllerImage}
      alt=""
      draggable={false}
      css={css`
        position: absolute;
        left: ${left(CONTROLLER.x)};
        top: ${top(CONTROLLER.y)};
        width: ${width(CONTROLLER.w)};
        height: ${height(CONTROLLER.h)};
      `}
    />
    {SPOTS.map((s) => {
      const on = (buttons & s.bit) !== 0;
      return (
        <button
          key={s.bit}
          type="button"
          title={nameOf(s.bit)}
          aria-label={nameOf(s.bit)}
          aria-pressed={on}
          onClick={() => onToggle(s.bit)}
          css={[
            buttonReset,
            css`
              left: ${left(s.x - s.w / 2)};
              top: ${top(s.y - s.h / 2)};
              width: ${width(s.w)};
              height: ${height(s.h)};
              border-radius: ${s.round ?? "50%"};
              transform: rotate(${s.rotate ?? 0}deg);
              background: ${on ? "rgba(255, 245, 170, 0.12)" : "transparent"};
              box-shadow: ${on ? `0 0 0 3px #fff6b0, 0 0 16px 8px ${GLOW}, 0 0 30px 12px ${GLOW_FAR}` : "none"};
              &:hover {
                box-shadow: ${on
                  ? `0 0 0 3px #fff6b0, 0 0 20px 10px ${GLOW}, 0 0 36px 14px ${GLOW_FAR}`
                  : "0 0 0 2px rgba(255, 255, 255, 0.7)"};
              }
            `,
          ]}
        />
      );
    })}
    {SHOULDERS.map((s) => {
      const on = (buttons & s.bit) !== 0;
      const w = s.h * s.aspect;
      return (
        <button
          key={s.bit}
          type="button"
          title={nameOf(s.bit)}
          aria-label={nameOf(s.bit)}
          aria-pressed={on}
          onClick={() => onToggle(s.bit)}
          css={[
            buttonReset,
            css`
              left: ${left(s.x - w / 2)};
              top: ${top(s.y - s.h / 2)};
              width: ${width(w)};
              height: ${height(s.h)};
              filter: ${on ? `drop-shadow(0 0 3px #fff6b0) drop-shadow(0 0 10px ${GLOW})` : "none"};
              &:hover {
                filter: ${on
                  ? `drop-shadow(0 0 3px #fff6b0) drop-shadow(0 0 14px ${GLOW})`
                  : "drop-shadow(0 0 3px rgba(255, 255, 255, 0.8))"};
              }
            `,
          ]}
        >
          <img
            src={s.src}
            alt=""
            draggable={false}
            css={css`
              width: 100%;
              height: 100%;
              display: block;
            `}
          />
        </button>
      );
    })}
  </div>
);
