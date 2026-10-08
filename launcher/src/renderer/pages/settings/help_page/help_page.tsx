import { css } from "@emotion/react";
import React from "react";

import { SupportBox } from "./support_box/support_box";

// Slippi's Help page also renders its FAQ.md below the support box. That FAQ is all
// about Slippi and Melee, so it was removed; add our own FAQ here once it is written.
export const HelpPage = React.memo(() => {
  return (
    <div>
      <div
        css={css`
          padding-top: 10px;
          padding-bottom: 20px;
        `}
      >
        <SupportBox />
      </div>
    </div>
  );
});
