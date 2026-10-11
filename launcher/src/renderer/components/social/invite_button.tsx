import MailOutlinedIcon from "@mui/icons-material/MailOutlined";
import Tooltip from "@mui/material/Tooltip";
import type { SocialPlayer } from "@social/types";
import { clsx } from "clsx";

import { inviteBlocked, useSocial, waitsForRoom } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";

import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

/**
 * The small envelope button: invites the player to the room the player is in (it needs an open
 * slot), or to a Direct 1v1 when not in a room.
 */
export const InviteButton = ({ player, className }: { player: SocialPlayer; className?: string }) => {
  const room = useSocial((s) => s.state.room);
  const waiting = useSocial((s) => waitsForRoom(s.pendingRoom, s.state.room));
  const online = useSocial((s) => s.state.online.some((p) => p.uid === player.uid));
  const { invite } = useSocialActions();
  // While a room the player asked for is opening, an invite is to that room.
  const full = !waiting && inviteBlocked(room) != null;
  const blocked = full || !online;
  const tip = full ? Messages.roomFull() : waiting || room ? Messages.inviteToRoom() : Messages.inviteToDirect();
  return (
    <Tooltip title={online ? tip : ""} placement="top" disableInteractive={true}>
      <span className={clsx(styles.inviteWrap, className)}>
        <button
          type="button"
          className={styles.inviteButton}
          aria-label={Messages.inviteToGame()}
          aria-disabled={blocked}
          disabled={blocked}
          onClick={(e) => {
            e.stopPropagation();
            void invite(player);
          }}
          onContextMenu={(e) => e.stopPropagation()}
        >
          <MailOutlinedIcon style={{ fontSize: 16 }} />
        </button>
      </span>
    </Tooltip>
  );
};

/** A display name with the connect code after it, lighter. */
export const PlayerName = ({ player }: { player: SocialPlayer }) => (
  <span className={styles.playerName}>
    <span className={styles.displayName}>{player.displayName}</span>
    {player.connectCode && <span className={styles.tag}>{player.connectCode}</span>}
  </span>
);
