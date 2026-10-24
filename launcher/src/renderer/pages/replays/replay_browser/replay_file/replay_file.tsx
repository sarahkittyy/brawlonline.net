import { css, keyframes } from "@emotion/react";
import styled from "@emotion/styled";
import CircularProgress from "@mui/material/CircularProgress";
import IconButton from "@mui/material/IconButton";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { DolphinStatus, useDolphinStore } from "@/lib/dolphin/use_dolphin_store";

import { DraggableFile } from "./draggable_file";
import { ReplayFileMessages as Messages } from "./replay_file.messages";
import type { PlayerInfo } from "./team_elements/team_elements";
import { TeamElements } from "./team_elements/team_elements";

export type ReplayDetail = {
  Icon: React.ComponentType;
  label: string;
};

export type ReplayFileAction = {
  Icon: React.ComponentType;
  primary?: boolean;
  disabled?: boolean;
  label: string;
  onClick: (e: React.MouseEvent<HTMLButtonElement, MouseEvent>) => void;
  isDolphinAction?: boolean; // Flag to indicate this button needs Dolphin status
  /** A ring around the icon: 0..1, or null for one still waiting to start. */
  progress?: number | null;
};

type ReplayFileProps = {
  fileName: string;
  players: PlayerInfo[][];
  details: ReplayDetail[];
  actions: ReplayFileAction[];
  backgroundImage?: string;
  onFileNameDragStart?: (event: React.DragEvent<HTMLDivElement>) => void;
  /** Non-zero: pulse gold (a new value pulses again); the replays the match history asked for. */
  highlightKey?: number;
  /** The playback Dolphin plays it (from this list): shown as selected. */
  playing?: boolean;
};

export const ReplayFile = React.memo(function ReplayFile({
  backgroundImage,
  actions,
  players,
  fileName,
  details,
  onFileNameDragStart,
  highlightKey = 0,
  playing = false,
}: ReplayFileProps) {
  return (
    <Outer
      backgroundImage={backgroundImage}
      selected={playing}
      data-replay-highlight={highlightKey ? highlightKey : undefined}
    >
      {highlightKey > 0 && <HighlightPulse key={highlightKey} />}
      <div
        css={css`
          display: flex;
          flex: 1;
          flex-direction: column;
          justify-content: center;
          padding: 10px;
          background-color: ${playing ? "rgba(180, 130, 176, 0.1)" : "transparent"};
          width: 100%;
        `}
      >
        <div
          css={css`
            display: flex;
            align-items: center;
            justify-content: space-between;
            width: 100%;
          `}
        >
          <TeamElements teams={players} />
          <div
            css={css`
              display: flex;
            `}
          >
            {actions.map((action, i) => (
              <ActionButton key={i} action={action} />
            ))}
          </div>
        </div>

        <div
          css={css`
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-top: 5px;
            font-size: 14px;
            white-space: nowrap;
            width: 100%;
          `}
        >
          <div
            css={css`
              display: flex;
              align-items: center;
              flex: 1;
            `}
          >
            {details.map(({ Icon, label }, i) => (
              <InfoItem key={i} Icon={Icon} label={label} />
            ))}
          </div>
          <DraggableFile
            onDragStart={onFileNameDragStart}
            css={css`
              max-width: 350px;
              white-space: nowrap;
              overflow: hidden;
              text-overflow: ellipsis;
              min-width: 0;
              opacity: 0.9;
              cursor: ${onFileNameDragStart ? "grab" : "inherit"};
              &:hover {
                opacity: 1;
                text-decoration: underline;
              }
            `}
          >
            {fileName}
          </DraggableFile>
        </div>
      </div>
    </Outer>
  );
});

const goldPulse = keyframes`
  0%, 50%, 100% {
    opacity: 0;
  }
  25%, 75% {
    opacity: 1;
  }
`;

// Two gold pulses over the row, after the list has had time to scroll to it.
const HighlightPulse = styled.div`
  position: absolute;
  inset: 0;
  z-index: 1;
  pointer-events: none;
  border-radius: 10px;
  opacity: 0;
  background: rgba(255, 196, 57, 0.14);
  box-shadow: inset 0 0 0 2px rgba(255, 196, 57, 0.95), inset 0 0 24px rgba(255, 196, 57, 0.45);
  animation: ${goldPulse} 1.6s ease-in-out 0.4s 1 both;
`;

const Outer = styled.div<{
  backgroundImage?: string;
  selected?: boolean;
}>`
  display: flex;
  position: relative;
  border-style: solid;
  border-width: 1px;
  border-color: ${(p) => (p.selected ? "rgba(255, 255, 255, 0.7)" : "rgba(159, 116, 192, 0.1)")};
  overflow: hidden;
  border-radius: 10px;
  height: 80px;
  margin: 10px;
  background: ${(p) =>
    p.backgroundImage
      ? `linear-gradient(to right, var(--surface-1) 20%, transparent 35%, transparent 65%, var(--surface-1) 80%)`
      : "var(--surface-1)"};
  will-change: border-color;
  &::before {
    z-index: -1;
    position: absolute;
    content: "";
    height: 100%;
    width: 100%;
    ${(p) => (p.backgroundImage ? `background-image: url("${p.backgroundImage}");` : "")}
    background-repeat: no-repeat;
    background-position: center;
    background-size: 60%;
    opacity: 0.25;
  }
`;

const InfoItem = React.memo(({ Icon, label }: { Icon: React.ComponentType; label: string }) => {
  return (
    <div
      css={css`
        display: flex;
        align-items: center;
        margin-right: 20px;
      `}
    >
      <div
        css={css`
          display: flex;
          margin-right: 5px;
          opacity: 0.6;
          svg {
            font-size: 16px;
          }
        `}
      >
        <Icon />
      </div>
      <div>{label}</div>
    </div>
  );
});

type ReplayActionButtonProps = Omit<React.ComponentProps<typeof IconButton>, "color"> & {
  label: string;
  color?: string;
};

// Wrapper component that handles dynamic Dolphin status for play button
const ActionButton = React.memo(({ action }: { action: ReplayFileAction }) => {
  const { label, onClick, Icon, disabled, primary, isDolphinAction, progress } = action;

  // Only subscribe to Dolphin status if this is a Dolphin action
  const playbackStatus = useDolphinStore((store) => store.playbackStatus);

  const finalLabel = isDolphinAction
    ? playbackStatus === DolphinStatus.READY
      ? label
      : Messages.dolphinIsUpdating()
    : label;

  const finalDisabled = isDolphinAction ? playbackStatus !== DolphinStatus.READY : disabled;

  return (
    <ReplayActionButton
      color={primary ? "var(--accent-primary-dark)" : undefined}
      label={finalLabel}
      onClick={onClick}
      disabled={finalDisabled}
    >
      <Icon />
      {progress !== undefined && (
        <CircularProgress
          size={34}
          thickness={2.5}
          variant={progress === null ? "indeterminate" : "determinate"}
          value={progress === null ? undefined : Math.max(progress * 100, 3)}
          css={css`
            position: absolute;
            inset: 0;
            color: inherit;
          `}
        />
      )}
    </ReplayActionButton>
  );
});

const ReplayActionButton = React.memo(({ label, color, onClick, ...rest }: ReplayActionButtonProps) => {
  return (
    <Tooltip title={label}>
      <span>
        <IconButton
          css={css`
            padding: 5px;
            margin: 0 5px;
            color: ${color ?? "var(--accent-secondary)"};
          `}
          size="large"
          onClick={(e) => {
            e.stopPropagation();
            onClick?.(e);
          }}
          {...rest}
        />
      </span>
    </Tooltip>
  );
});
