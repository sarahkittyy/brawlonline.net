import type { DolphinSetupPhase } from "@dolphin/types";
import { css } from "@emotion/react";
import ButtonBase from "@mui/material/ButtonBase";
import React from "react";

import { bodyFont } from "@/styles/with_font";

import { PlayButtonMessages as Messages } from "./play_button.messages";
import { PlayIcon } from "./play_icon";

type MainButtonProps = React.ComponentProps<typeof ButtonBase> & {
  fillPercent?: number;
  /** Fade the button while it is disabled (not while a set-up step shows its progress). */
  dimWhenDisabled?: boolean;
};

const MainButton = React.memo((props: MainButtonProps) => {
  const { children, fillPercent, dimWhenDisabled = true, ...rest } = props;
  return (
    <ButtonBase
      {...rest}
      css={css`
        transition: opacity 0.2s ease-in-out;
        &:disabled {
          opacity: ${dimWhenDisabled ? 0.5 : 1};
        }
        &:hover {
          /* The game theme highlights the frame instead of fading the button. */
          opacity: var(--theme-play-hover-opacity, 0.8);
        }
        /* Keep the press ripple inside the game frame's face (not over its transparent corners). */
        & > .MuiTouchRipple-root {
          inset: var(--theme-button-width, 0px);
        }
      `}
    >
      <PlayIcon fillPercent={fillPercent}>{children}</PlayIcon>
    </ButtonBase>
  );
});

type PlayButtonProps = Omit<MainButtonProps, "children" | "fillPercent" | "dimWhenDisabled">;

export const PlayButton = React.memo((props: PlayButtonProps) => {
  return <MainButton {...props}>{Messages.play()}</MainButton>;
});

/** The button's words for a set-up step; Slippi's "Updating" for steps without a name. */
export function setupPhaseLabel(phase: DolphinSetupPhase | undefined): string {
  switch (phase) {
    case "installDolphin":
      return Messages.installingDolphin();
    case "copyUserFolder":
      return Messages.copyingFiles();
    case "downloadProjectPlus":
      return Messages.downloadingProjectPlus();
    case "verifyProjectPlus":
      return Messages.verifyingProjectPlus();
    case "extractProjectPlus":
      return Messages.extractingProjectPlus();
    case "prepareSdCard":
      return Messages.preparingSdCard();
    default:
      return Messages.updating();
  }
}

type UpdatingButtonProps = Omit<MainButtonProps, "children" | "dimWhenDisabled"> & {
  /** The step that is running. Without one the button says "Updating", as Slippi's does. */
  phase?: DolphinSetupPhase;
};

export const UpdatingButton = React.memo(({ phase, ...props }: UpdatingButtonProps) => {
  if (!phase) {
    return (
      <MainButton disabled={true} {...props}>
        <span
          css={css`
            font-size: 0.9em;
          `}
        >
          {Messages.updating()}
        </span>
      </MainButton>
    );
  }
  // A named step: its name, and how far along it is under it. Not faded (the fill shows the
  // progress), and white with a dark outline, so it reads on the plain frame too (before P+'s
  // files give the theme its colours).
  const percent = Math.floor(Math.min(1, Math.max(0, props.fillPercent ?? 0)) * 100);
  return (
    <MainButton disabled={true} dimWhenDisabled={false} {...props}>
      <span
        css={css`
          display: flex;
          flex-direction: column;
          align-items: center;
          line-height: 1.15;
          color: #fff;
          text-shadow: 0 0 2px #000, 0 1px 2px #000, 1px 0 1px #000, -1px 0 1px #000;
        `}
      >
        <span
          css={css`
            /* The menu font, as written: the wide title capitals do not fit "Downloading Project+". */
            font-family: ${bodyFont};
            text-transform: none;
            font-size: 12px;
            max-width: 156px;
            overflow: hidden;
            text-overflow: ellipsis;
          `}
        >
          {setupPhaseLabel(phase)}
        </span>
        <span
          css={css`
            font-size: 14px;
          `}
        >
          {percent}%
        </span>
      </span>
    </MainButton>
  );
});
