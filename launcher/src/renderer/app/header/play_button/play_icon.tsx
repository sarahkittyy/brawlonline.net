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
        button:hover > & {
          border-image-source: var(--theme-buttonSelected-source, var(--theme-button-source, none));
        }
      `}
    >
      <div
        css={css`
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
