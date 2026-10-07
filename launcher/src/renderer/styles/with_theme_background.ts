import { css } from "@emotion/react";

/**
 * Page background. With extracted game assets it is P+'s menu background
 * gradient with its grid tile (see styles/game_theme.tsx); without them it stays
 * the plain fallback colour. Replaces Slippi's logo watermark.
 */
export const withThemeBackground = css`
  background: var(--theme-background, none);
  background-color: var(--theme-page-background, transparent);
`;
