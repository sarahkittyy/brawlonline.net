/**
 * GameCube buttons as icons ("L + R + Z"), for Settings > Clips > Manual Clip Hotkey. The icons
 * are Zacksly's "GameCube Button Icons and Controls" (Buttons Colored, Hard Edge), each SVG's
 * view box cropped to its button and Start's "START/PAUSE" caption put inside the button. They
 * are images, not inline SVGs: every one of them names its classes and gradients the same.
 */
import type { GcButton } from "@common/clip_hotkey";
import { hotkeyButtons } from "@common/clip_hotkey";
import { css } from "@emotion/react";
import React from "react";

import aIcon from "./images/A.svg";
import bIcon from "./images/B.svg";
import dPadDownIcon from "./images/DPadDown.svg";
import dPadLeftIcon from "./images/DPadLeft.svg";
import dPadRightIcon from "./images/DPadRight.svg";
import dPadUpIcon from "./images/DPadUp.svg";
import lIcon from "./images/L.svg";
import rIcon from "./images/R.svg";
import startIcon from "./images/Start.svg";
import xIcon from "./images/X.svg";
import yIcon from "./images/Y.svg";
import zIcon from "./images/Z.svg";

/** Each icon and its height at size 1 (in rem): A the largest, as on the controller. */
const ICONS: Record<number, { src: string; height: number }> = {
  0x0040: { src: lIcon, height: 1.75 },
  0x0020: { src: rIcon, height: 1.75 },
  0x0010: { src: zIcon, height: 1.25 },
  0x0100: { src: aIcon, height: 1.9 },
  0x0200: { src: bIcon, height: 1.5 },
  0x0400: { src: xIcon, height: 1.35 },
  0x0800: { src: yIcon, height: 1.35 },
  0x1000: { src: startIcon, height: 1.25 },
  0x0008: { src: dPadUpIcon, height: 1.75 },
  0x0004: { src: dPadDownIcon, height: 1.75 },
  0x0001: { src: dPadLeftIcon, height: 1.75 },
  0x0002: { src: dPadRightIcon, height: 1.75 },
};

export const GcButtonIcon = ({ button, size = 1 }: { button: GcButton; size?: number }) => {
  const icon = ICONS[button.bit];
  if (!icon) {
    return <span>{button.name}</span>;
  }
  return (
    <img
      src={icon.src}
      alt={button.name}
      title={button.name}
      draggable={false}
      css={css`
        height: ${icon.height * size}rem;
        display: block;
      `}
    />
  );
};

/** The buttons of a hotkey (GcButton bits), with "+" between them. */
export const GcButtonRow = ({ buttons, size = 1 }: { buttons: number; size?: number }) => (
  <div
    css={css`
      display: inline-flex;
      align-items: center;
      gap: ${0.4 * size}rem;
    `}
  >
    {hotkeyButtons(buttons).map((b, i) => (
      <React.Fragment key={b.bit}>
        {i > 0 && (
          <span
            css={css`
              font-size: ${1.1 * size}rem;
              font-weight: 600;
              opacity: 0.8;
            `}
          >
            +
          </span>
        )}
        <GcButtonIcon button={b} size={size} />
      </React.Fragment>
    ))}
  </div>
);
