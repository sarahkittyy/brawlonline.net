import { css } from "@emotion/react";
import React from "react";

import { titleFont } from "@/styles/with_font";

/**
 * The Play button face. Slippi draws its own branded SVG shape here; we use the
 * main menu button frame extracted from the user's Brawl disc (CSS variables set
 * by styles/game_theme.tsx) and a plain outlined box until it is available.
 * `fillPercent` fills it from the left while Dolphin is being prepared.
 */
export const PlayIcon = ({ children, fillPercent = 1 }: React.PropsWithChildren<{ fillPercent?: number }>) => {
  const offset = `${(fillPercent * 100).toFixed(2)}%`;
  return (
    <div
      css={css`
        position: relative;
        width: 166px;
        height: 45px;
        box-sizing: border-box;
        display: flex;
        align-items: center;
        justify-content: center;
        border: 2px solid var(--accent-primary);
        border-color: var(--accent-primary);
        /* With the game frame the border is as wide as the frame's corners: a thinner border would let
           the fill below show through the frame's transparent rounded corners. */
        border-width: var(--theme-button-width, 2px);
        border-image-source: var(--theme-button-source, none);
        border-image-slice: var(--theme-button-slice, 0);
        border-image-width: var(--theme-button-width, 2px);
        border-image-repeat: stretch;
        background: linear-gradient(
          to right,
          var(--accent-primary-dark) 0%,
          var(--accent-primary-dark) ${offset},
          transparent ${offset},
          transparent 100%
        );
        background-clip: padding-box;
        /* The game frame's face is opaque and hides that fill: shade the part not filled yet instead. */
        &::before {
          content: "";
          position: absolute;
          top: 0;
          right: 0;
          bottom: 0;
          left: ${offset};
          background: var(--theme-progress-track, transparent);
          pointer-events: none;
        }
        button:hover > & {
          border-image-source: var(--theme-buttonHover-source, var(--theme-button-source, none));
        }
      `}
    >
      {fillPercent < 1 && (
        // The game's frame (border-image with `fill`) covers the background above, so the filled
        // part is also drawn over the face, where it shows in both looks.
        <div
          data-testid="play-button-fill"
          css={css`
            position: absolute;
            left: 0;
            top: 0;
            bottom: 0;
            width: ${offset};
            background-color: currentColor;
            opacity: 0.2;
            pointer-events: none;
          `}
        />
      )}
      <div
        css={css`
          position: relative;
          font-family: ${titleFont};
          font-weight: bold;
          text-transform: uppercase;
          white-space: nowrap;
          text-shadow: 0px -1px #444;
          font-size: 20px;
          line-height: 45px;
        `}
      >
        {children}
      </div>
    </div>
  );
};
