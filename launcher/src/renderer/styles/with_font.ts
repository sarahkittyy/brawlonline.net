/** Body and title fonts. CSS variables set by the theme (styles/game_theme.ts). */
export const SYSTEM_FONT_STACK = 'system-ui, -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif';

export const bodyFont = `var(--font-body, ${SYSTEM_FONT_STACK})`;
export const titleFont = `var(--font-title, ${SYSTEM_FONT_STACK})`;
