import { css } from "@emotion/react";
import React from "react";

import { CommonProblems } from "./common_problems/common_problems";
import { SupportBox } from "./support_box/support_box";

// Slippi's Help page also renders its FAQ.md below the support box. That FAQ is all
// about Slippi and Melee, so it was removed; our own common problems are below it instead.
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
      <CommonProblems />
    </div>
  );
});
