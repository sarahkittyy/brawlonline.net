import TimerOutlinedIcon from "@mui/icons-material/TimerOutlined";
import type { QueueMode, QueueState, SocialPlayer } from "@social/types";
import { clsx } from "clsx";
import React from "react";

import { MainPicture } from "@/components/main_picture/main_picture";
import { useStockIcon } from "@/components/profile/character_icon";
import { QueueMessages } from "@/lib/queue/queue.messages";
import { formatClock, formatEta } from "@/lib/queue/queue_eta";
import { useRoomList } from "@/lib/social/use_room_list";
import { useSocial } from "@/lib/social/use_social";
import { useNow } from "@/lib/spectate/use_spectate";
import { SpectateMessages } from "@/pages/home/spectate/spectate.messages";

import styles from "./queue_box.module.css";

export function queueModeName(mode: QueueMode): string {
  switch (mode) {
    case "ranked":
      return SpectateMessages.modeRanked();
    case "unranked":
      return SpectateMessages.modeUnranked();
    case "direct":
      return SpectateMessages.modeDirect();
    case "twos":
      return SpectateMessages.modeTwos();
  }
}

/**
 * The player's search or match, above their card in the sidebar (docs/design/launcher-redesign.md §7). The game
 * searches (the player waits in Online Training, as before); this only shows it, on every page, so a player browsing
 * the launcher sees at once when their game is found. Searching: the mode, how long, and the ETA (2v2: the partner).
 * In a match: the opponent and the game's score, in the selected-page frame.
 */
export const QueueBox = React.memo(function QueueBox() {
  const queue = useSocial((s) => s.state.queue ?? null);
  if (!queue) {
    return null;
  }
  return <QueueBoxView queue={queue} />;
});

export const QueueBoxView = ({ queue, eta }: { queue: QueueState; eta?: number | null }) => {
  if (queue.state === "playing") {
    return <PlayingBox queue={queue} />;
  }
  return <SearchingBox queue={queue} eta={eta} />;
};

const SearchingBox = ({
  queue,
  eta: etaOverride,
}: {
  queue: Extract<QueueState, { state: "searching" }>;
  eta?: number | null;
}) => {
  const now = useNow(1000);
  const { query } = useRoomList();
  const since = Date.parse(queue.since);
  const elapsed = Number.isFinite(since) ? (now - since) / 1000 : 0;
  const etas = query.data?.queueEta;
  const eta =
    etaOverride !== undefined
      ? etaOverride
      : etas && (queue.mode === "ranked" || queue.mode === "unranked")
      ? etas[queue.mode]
      : undefined;
  return (
    <div className={styles.box} data-testid="queue-box" data-state="searching">
      <span className={styles.pulse} aria-hidden={true} />
      <div className={styles.text}>
        <div className={styles.mode}>{queueModeName(queue.mode)}</div>
        <div className={styles.sub}>{QueueMessages.searching(formatClock(elapsed))}</div>
        {queue.partner ? (
          <div className={clsx(styles.sub, styles.partner)}>
            <PlayerPicture player={queue.partner} size={18} />
            <span className={styles.ellipsis}>{queue.partner.displayName}</span>
          </div>
        ) : (
          eta !== undefined && (
            <div className={clsx(styles.sub, styles.eta)}>
              <TimerOutlinedIcon className={styles.etaIcon} />
              {formatEta(eta)}
            </div>
          )
        )}
      </div>
    </div>
  );
};

const PlayingBox = ({ queue }: { queue: Extract<QueueState, { state: "playing" }> }) => {
  const opponents = queue.opponents.map((p) => p.displayName).join(` ${SpectateMessages.and()} `);
  return (
    <div className={clsx(styles.box, styles.playing)} data-testid="queue-box" data-state="playing">
      {queue.opponents[0] && <PlayerPicture player={queue.opponents[0]} size={32} />}
      <div className={styles.text}>
        <div className={styles.mode}>{queueModeName(queue.mode)}</div>
        <div className={clsx(styles.sub, styles.ellipsis)}>
          {SpectateMessages.vs()} {opponents}
        </div>
        <div className={styles.sub}>{SpectateMessages.gameScore(queue.game, `${queue.wins[0]}-${queue.wins[1]}`)}</div>
      </div>
    </div>
  );
};

/** A player's main's stock icon; without one (no main shown, no disc) the dotted outline, as the player's own card. */
const PlayerPicture = ({ player, size }: { player: SocialPlayer; size: number }) => {
  const url = useStockIcon(player.main ?? "");
  return <MainPicture url={player.main ? url ?? null : null} size={size} />;
};
