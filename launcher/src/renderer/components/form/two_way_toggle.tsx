// A two-sided toggle bar: two labels side by side and a highlight that slides under the chosen one
// (Sarah's board: "a two state toggle bar. like a left and right side and the highlighted part moves
// back and forth"). Both halves are as wide as the wider label, so the highlight's slide is exactly one
// half. First used for the EmissaryRecomp card's Dolphin / Emissary choice.
import { css } from "@emotion/react";
import { alpha, useTheme } from "@mui/material/styles";

type TwoWayToggleProps = {
  left: string;
  right: string;
  /** The right side is chosen. */
  value: boolean;
  onChange: (right: boolean) => void;
  disabled?: boolean;
  "aria-label"?: string;
};

const PAD = 3;

export const TwoWayToggle = ({
  left,
  right,
  value,
  onChange,
  disabled,
  "aria-label": ariaLabel,
}: TwoWayToggleProps) => {
  const theme = useTheme();
  const side = (label: string, isRight: boolean) => {
    const chosen = value === isRight;
    return (
      <button
        type="button"
        role="radio"
        aria-checked={chosen}
        disabled={disabled}
        onClick={() => !chosen && onChange(isRight)}
        css={css`
          position: relative;
          z-index: 1;
          border: 0;
          background: none;
          font: inherit;
          font-size: 14px;
          font-weight: 600;
          padding: 3px 14px;
          border-radius: 999px;
          white-space: nowrap;
          cursor: ${disabled || chosen ? "default" : "pointer"};
          color: ${chosen ? theme.palette.primary.contrastText : theme.palette.text.primary};
          opacity: ${chosen ? 1 : 0.65};
          transition: color 180ms ease, opacity 180ms ease;
          &:hover:not(:disabled) {
            opacity: 1;
          }
          &:focus-visible {
            outline: 2px solid ${alpha(theme.palette.primary.main, 0.8)};
            outline-offset: 1px;
          }
        `}
      >
        {label}
      </button>
    );
  };
  return (
    <div
      role="radiogroup"
      aria-label={ariaLabel}
      css={css`
        position: relative;
        display: inline-grid;
        grid-template-columns: 1fr 1fr;
        padding: ${PAD}px;
        border-radius: 999px;
        background: rgba(0, 0, 0, 0.35);
        border: 1px solid rgba(255, 255, 255, 0.12);
        opacity: ${disabled ? 0.5 : 1};
      `}
    >
      <span
        aria-hidden={true}
        css={css`
          position: absolute;
          top: ${PAD}px;
          bottom: ${PAD}px;
          left: ${PAD}px;
          width: calc(50% - ${PAD}px);
          border-radius: 999px;
          background: ${theme.palette.primary.main};
          box-shadow: 0 1px 4px rgba(0, 0, 0, 0.35);
          transform: translateX(${value ? "100%" : "0"});
          transition: transform 220ms cubic-bezier(0.4, 0, 0.2, 1);
        `}
      />
      {side(left, false)}
      {side(right, true)}
    </div>
  );
};
