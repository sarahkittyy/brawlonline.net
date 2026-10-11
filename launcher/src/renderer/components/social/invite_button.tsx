import PersonAddAlt1OutlinedIcon from "@mui/icons-material/PersonAddAlt1Outlined";
import Tooltip from "@mui/material/Tooltip";
import type { SocialPlayer } from "@social/types";
import { clsx } from "clsx";

import { inviteBlocked, useSocial } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";

import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

/**
 * The small "+ person" button: invites the player to the room the player is in (it needs an open
 * slot), or to a Direct 1v1 when not in a room.
 */
export const InviteButton = ({ player, className }: { player: SocialPlayer; className?: string }) => {
  const room = useSocial((s) => s.state.room);
  const online = useSocial((s) => s.state.online.some((p) => p.uid === player.uid));
  const { invite } = useSocialActions();
  const blocked = inviteBlocked(room) != null || !online;
  const tip = inviteBlocked(room) ? Messages.roomFull() : room ? Messages.inviteToRoom() : Messages.inviteToDirect();
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
          <PersonAddAlt1OutlinedIcon style={{ fontSize: 16 }} />
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
