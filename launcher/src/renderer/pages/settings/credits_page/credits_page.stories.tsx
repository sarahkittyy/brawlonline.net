import { css } from "@emotion/react";

import { CreditsPage } from "./credits_page";

export default {
  title: "pages/Settings/CreditsPage",
  argTypes: {},
};

/** Settings > Credits, at the settings content column's width, with the surface colours that
 * game_theme.tsx sets from P+ 3.x's palette (as in components/Toggle). */
export const PPlusTheme = () => (
  <div
    css={css`
      --surface-3: #2773a1;
      background: #092f2a;
      max-width: 800px;
      padding: 20px;
    `}
  >
    <CreditsPage />
  </div>
);
