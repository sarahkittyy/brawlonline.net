import { css } from "@emotion/react";
import Typography from "@mui/material/Typography";
import React from "react";

type SettingItemProps = {
  name?: React.ReactNode | string;
  description?: React.ReactNode | string;
  /** The control on the right of the name and description, its right edge on the switches' (Toggle) instead of below. */
  inline?: boolean;
  children?: React.ReactNode;
};

/**
 * Where a Toggle's switch ends: MUI's Switch is 58 px wide but its thumb, at its right when on, stops 9 px
 * short of that. An inline control ends there too.
 */
export const SWITCH_RIGHT_INSET = 9;

export const SettingItem = ({ name, description, inline, children }: SettingItemProps) => {
  const text = (
    <div
      css={css`
        padding-bottom: ${inline ? 0 : 10}px;
        min-width: 0;
      `}
    >
      {name && (
        <Typography
          variant="subtitle1"
          css={css`
            text-transform: capitalize;
          `}
        >
          {name}
        </Typography>
      )}
      {description && <Typography variant="caption">{description}</Typography>}
    </div>
  );
  if (inline) {
    return (
      <div
        css={css`
          margin: 20px 0;
          display: flex;
          align-items: center;
          justify-content: space-between;
          gap: 24px;
        `}
      >
        {text}
        <div
          css={css`
            flex-shrink: 0;
            display: flex;
            align-items: center;
            margin-right: ${SWITCH_RIGHT_INSET}px;
          `}
        >
          {children}
        </div>
      </div>
    );
  }
  return (
    <div
      css={css`
        margin: 20px 0;
      `}
    >
      {text}
      {children}
    </div>
  );
};
