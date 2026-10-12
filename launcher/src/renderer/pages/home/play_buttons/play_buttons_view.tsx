import type { QueueTarget } from "@dolphin/hand_off_target";
import { QUEUE_RANKED, QUEUE_UNRANKED } from "@dolphin/hand_off_target";
import EmojiEventsRoundedIcon from "@mui/icons-material/EmojiEventsRounded";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import React from "react";

import { PlayButtonsMessages as Messages } from "./play_buttons.messages";
import styles from "./play_buttons.module.css";

export type PlayButtonsViewProps = {
  /** Players searching each queue; absent while unknown (logged out, the list not read yet). */
  queues?: { unranked: number; ranked: number };
  /** Players in a match of each queue; absent while unknown or from an older server (then not shown). */
  playing?: { unranked: number; ranked: number };
  /** The queue being handed to the game (its button shows a spinner, both wait). */
  starting?: QueueTarget;
  onPlay: (queue: QueueTarget) => void;
};

/**
 * Home's Play unranked / Play ranked, beside the Overview tab, each with its queue's count and the
 * players in its matches under it ("6 in queue · 14 in game").
 */
export const PlayButtonsView = ({ queues, playing, starting, onPlay }: PlayButtonsViewProps) => (
  <div className={styles.buttons}>
    <PlayButton
      label={Messages.playUnranked()}
      tooltip={Messages.playUnrankedTooltip()}
      icon={<PlayArrowRoundedIcon />}
      count={queues?.unranked}
      inGame={playing?.unranked}
      starting={starting === QUEUE_UNRANKED}
      disabled={starting != null}
      onClick={() => onPlay(QUEUE_UNRANKED)}
      testId="play-unranked"
    />
    <PlayButton
      label={Messages.playRanked()}
      tooltip={Messages.playRankedTooltip()}
      icon={<EmojiEventsRoundedIcon />}
      count={queues?.ranked}
      inGame={playing?.ranked}
      starting={starting === QUEUE_RANKED}
      disabled={starting != null}
      onClick={() => onPlay(QUEUE_RANKED)}
      testId="play-ranked"
    />
  </div>
);

const PlayButton = ({
  label,
  tooltip,
  icon,
  count,
  inGame,
  starting,
  disabled,
  onClick,
  testId,
}: {
  label: string;
  tooltip: string;
  icon: React.ReactNode;
  count?: number;
  inGame?: number;
  starting: boolean;
  disabled: boolean;
  onClick: () => void;
  testId: string;
}) => (
  <div className={styles.play}>
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
          sx={{ textTransform: "none", fontWeight: 600, borderRadius: "8px", px: 1.5 }}
        >
          {label}
        </Button>
      </span>
    </Tooltip>
    {/* Kept in the layout while unknown, so the row does not move when the count comes in. */}
    <span
      className={styles.count}
      data-testid={`${testId}-count`}
      style={{ visibility: count == null ? "hidden" : undefined }}
    >
      {Messages.inQueue(count ?? 0)}
      {count != null && inGame != null && ` · ${Messages.inGame(inGame)}`}
    </span>
  </div>
);
