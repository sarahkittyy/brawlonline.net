import { css } from "@emotion/react";
import CircularProgress from "@mui/material/CircularProgress";
import LinearProgress from "@mui/material/LinearProgress";
import React from "react";

import { Message } from "../message";
import { LoadingScreenMessages as Messages } from "./loading_screen.messages";

export const LoadingScreen = ({
  message = Messages.justASec(),
  style,
  className,
}: {
  className?: string;
  message?: string;
  style?: React.CSSProperties;
}) => {
  return (
    <Message className={className} style={style} icon={<CircularProgress color="inherit" />}>
      <p
        css={css`
          text-align: center;
          max-width: 650px;
          word-break: break-word;
          line-height: 1.4em;
          padding: 0 50px;
        `}
      >
        {message}
      </p>
    </Message>
  );
};

export function LoadingScreenWithProgress({ current = 0, total = 100 }: { current?: number; total?: number }) {
  return (
    <Message icon={<CircularProgress color="inherit" />}>
      <ProgressBar current={current} total={total} style={{ marginTop: 30 }} />
    </Message>
  );
}

/** The loading screen's progress bar on its own (also used by the ISO check). */
export function ProgressBar({
  current = 0,
  total = 100,
  style,
}: {
  current?: number;
  total?: number;
  style?: React.CSSProperties;
}) {
  return (
    <div
      style={{
        color: "var(--off-white)",
        padding: 3,
        borderRadius: 10,
        borderStyle: "solid",
        borderWidth: 2,
        width: 180,
        ...style,
      }}
    >
      <LinearProgress
        variant="determinate"
        value={total > 0 ? Math.min(100, (current / total) * 100) : 0}
        sx={{
          borderRadius: 10,
          height: 8,
          "& .MuiLinearProgress-bar": { borderRadius: 10, transitionDuration: "50ms" },
        }}
        color="inherit"
      />
    </div>
  );
}
