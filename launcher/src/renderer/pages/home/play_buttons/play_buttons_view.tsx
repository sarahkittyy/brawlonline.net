import type { QueueTarget } from "@dolphin/hand_off_target";
import { QUEUE_RANKED, QUEUE_TWOS, QUEUE_UNRANKED } from "@dolphin/hand_off_target";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import GroupsRoundedIcon from "@mui/icons-material/GroupsRounded";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { PlayButtonsMessages as Messages } from "./play_buttons.messages";
import styles from "./play_buttons.module.css";

export type PlayButtonsViewProps = {
  /** The queue being handed to the game (its button shows a spinner, both wait). */
  starting?: QueueTarget;
  onPlay: (queue: QueueTarget) => void;
};

/**
 * Home's Play unranked / Play ranked / Play 2v2, beside the Overview tab. No player counts (until 2026-11-09
 * "6 in queue · 14 in game" under each): an empty queue put players off searching, which kept it empty.
 */
export const PlayButtonsView = ({ starting, onPlay }: PlayButtonsViewProps) => (
  <div className={styles.buttons}>
    <PlayButton
      label={Messages.playUnranked()}
      tooltip={Messages.playUnrankedTooltip()}
      icon={<PlayArrowRoundedIcon />}
      starting={starting === QUEUE_UNRANKED}
      disabled={starting != null}
      onClick={() => onPlay(QUEUE_UNRANKED)}
      testId="play-unranked"
    />
    <PlayButton
      label={Messages.playRanked()}
      tooltip={Messages.playRankedTooltip()}
      icon={<EmojiEventsRoundedIcon />}
      starting={starting === QUEUE_RANKED}
      disabled={starting != null}
      onClick={() => onPlay(QUEUE_RANKED)}
      testId="play-ranked"
    />
    <PlayButton
      label={Messages.playTwos()}
      tooltip={Messages.playTwosTooltip()}
      icon={<GroupsRoundedIcon />}
      starting={starting === QUEUE_TWOS}
      disabled={starting != null}
      onClick={() => onPlay(QUEUE_TWOS)}
      testId="play-twos"
    />
  </div>
);

const PlayButton = ({
  label,
  tooltip,
  icon,
  starting,
  disabled,
  onClick,
  testId,
}: {
  label: string;
  tooltip: string;
  icon: React.ReactNode;
  starting: boolean;
  disabled: boolean;
  onClick: () => void;
  testId: string;
}) => (
  <Tooltip title={tooltip} placement="bottom" enterDelay={400} disableInteractive={true}>
    <span>
      <Button
        variant="contained"
        color="secondary"
        size="small"
        disableElevation={true}
        disabled={disabled}
        startIcon={starting ? <CircularProgress color="inherit" size={14} /> : icon}
        onClick={onClick}
        data-testid={testId}
        sx={{
          textTransform: "none",
          fontWeight: 600,
          borderRadius: "8px",
          px: 1.5,
        }}
      >
        {label}
      </Button>
    </span>
  </Tooltip>
);
