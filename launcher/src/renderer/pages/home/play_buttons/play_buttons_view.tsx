import type { QueueTarget } from "@dolphin/hand_off_target";
import { QUEUE_RANKED, QUEUE_TWOS, QUEUE_UNRANKED } from "@dolphin/hand_off_target";
import TimerOutlinedIcon from "@mui/icons-material/TimerOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";

import { formatEta } from "@/lib/queue/queue_eta";

import { PlayButtonsMessages as Messages } from "./play_buttons.messages";
import styles from "./play_buttons.module.css";

export type PlayButtonsViewProps = {
  /** Players searching 2v2 (`GET /v1/rooms` `queues.twos`); absent while unknown (logged out, the list not read yet). */
  twosQueue?: number;
  /** Players in a 2v2 match (`playing.twos`); absent while unknown or from an older server (then not shown). */
  twosPlaying?: number;
  /** How long Unranked and Ranked searches wait this hour (`queueEta`); absent while unknown. null: "10+ min". */
  etas?: { unranked: number | null; ranked: number | null };
  /** The queue being handed to the game (its button shows a spinner, both wait). */
  starting?: QueueTarget;
  onPlay: (queue: QueueTarget) => void;
};

/**
 * Play's three buttons, stacked on the left of the ranked card (docs/design/launcher-redesign.md §8): each with its
 * status on its right. Unranked and Ranked show the stopwatch and the hour's ETA (players searching is 0-2 at most
 * hours, so it told nothing; Sarah's board, 2026-11-14); 2v2 keeps its counts ("2 in queue · 8 in game").
 */
export const PlayButtonsView = ({ twosQueue, twosPlaying, etas, starting, onPlay }: PlayButtonsViewProps) => (
  <div className={styles.buttons}>
    <div className={styles.play}>
      <PlayButton
        label={Messages.playUnranked()}
        tooltip={Messages.playUnrankedTooltip()}
        starting={starting === QUEUE_UNRANKED}
        disabled={starting != null}
        onClick={() => onPlay(QUEUE_UNRANKED)}
        testId="play-unranked"
      />
      <Eta seconds={etas?.unranked} testId="play-unranked-eta" />
    </div>
    <div className={styles.play}>
      <PlayButton
        label={Messages.playRanked()}
        tooltip={Messages.playRankedTooltip()}
        starting={starting === QUEUE_RANKED}
        disabled={starting != null}
        onClick={() => onPlay(QUEUE_RANKED)}
        testId="play-ranked"
      />
      <Eta seconds={etas?.ranked} testId="play-ranked-eta" />
    </div>
    <div className={styles.play}>
      <PlayButton
        label={Messages.playTwos()}
        tooltip={Messages.playTwosTooltip()}
        starting={starting === QUEUE_TWOS}
        disabled={starting != null}
        onClick={() => onPlay(QUEUE_TWOS)}
        testId="play-twos"
      />
      {/* Kept in the layout while unknown, so the row does not move when the count comes in. */}
      <span
        className={styles.count}
        data-testid="play-twos-count"
        style={{ visibility: twosQueue == null ? "hidden" : undefined }}
      >
        {Messages.inQueue(twosQueue ?? 0)}
        {twosQueue != null && twosPlaying != null && ` · ${Messages.inGame(twosPlaying)}`}
      </span>
    </div>
  </div>
);

/** The stopwatch and the ETA; nothing while it is unknown. */
const Eta = ({ seconds, testId }: { seconds: number | null | undefined; testId: string }) => (
  <Tooltip title={Messages.etaTooltip()} placement="bottom" disableInteractive={true}>
    <span
      className={styles.count}
      data-testid={testId}
      style={{ visibility: seconds === undefined ? "hidden" : undefined }}
    >
      <TimerOutlinedIcon className={styles.etaIcon} />
      {seconds !== undefined && formatEta(seconds)}
    </span>
  </Tooltip>
);

const PlayButton = ({
  label,
  tooltip,
  starting,
  disabled,
  onClick,
  testId,
}: {
  label: string;
  tooltip: string;
  starting: boolean;
  disabled: boolean;
  onClick: () => void;
  testId: string;
}) => (
  <Tooltip title={tooltip} placement="bottom" enterDelay={400} disableInteractive={true}>
    <span className={styles.buttonSlot}>
      <Button
        variant="contained"
        color="primary"
        disableElevation={true}
        disabled={disabled}
        onClick={onClick}
        data-testid={testId}
        className={styles.button}
      >
        {/* The spinner takes the label's place, so the button keeps its width. */}
        <span style={{ visibility: starting ? "hidden" : undefined }}>{label}</span>
        {starting && <CircularProgress color="inherit" size={16} className={styles.spinner} />}
      </Button>
    </span>
  </Tooltip>
);
