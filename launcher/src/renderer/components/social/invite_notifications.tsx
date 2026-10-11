import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import type { Invite } from "@social/types";
import { clsx } from "clsx";
import React from "react";

import { useToasts } from "@/lib/hooks/use_toasts";
import { useHandOff } from "@/lib/social/use_hand_off";
import { useSocial } from "@/lib/social/use_social";

import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

const modeLabel = { "1v1": "1v1", ffa: "FFA", teams: "Teams" } as const;

/** "bob invited you. Join?" in the bottom right, one per inviter, newest at the bottom. */
export const InviteNotifications = React.memo(function InviteNotifications() {
  const invites = useSocial((s) => s.state.invites);
  if (invites.length === 0) {
    return null;
  }
  return (
    <div className={styles.invites}>
      {invites.slice(-4).map((invite) => (
        <InviteCard key={invite.id} invite={invite} />
      ))}
    </div>
  );
});

const InviteCard = ({ invite }: { invite: Invite }) => {
  const { showError } = useToasts();
  const handOff = useHandOff();
  const name = invite.from.displayName;

  const answer = async (accept: boolean) => {
    const result = await window.electron.social.answerInvite(invite.id, accept);
    if (!result.ok) {
      showError(result.message);
      return;
    }
    if (accept) {
      await handOff(invite.kind === "room" && invite.room ? invite.room.code : invite.from.connectCode);
    }
  };

  return (
    <div className={styles.invite} role="alertdialog" data-testid={`invite-${invite.from.uid}`}>
      <div>{invite.kind === "room" ? Messages.invitedYouRoom(name) : Messages.invitedYouDirect(name)}</div>
      <div className={styles.inviteDetail}>
        {invite.room
          ? `${modeLabel[invite.room.mode]} · ${invite.room.players}/${invite.room.openSlots}`
          : invite.from.connectCode}
        {" · "}
        {Messages.join()}
      </div>
      <div className={styles.inviteActions}>
        <button type="button" className={styles.yes} onClick={() => void answer(true)}>
          {Messages.yes()}
        </button>
        <button type="button" className={styles.no} onClick={() => void answer(false)}>
          {Messages.no()}
        </button>
      </div>
      <button
        type="button"
        className={clsx(styles.iconButton, styles.inviteClose)}
        aria-label={Messages.dismiss()}
        onClick={() => void window.electron.social.dismissInvite(invite.id)}
      >
        <CloseRoundedIcon style={{ fontSize: 15 }} />
      </button>
    </div>
  );
};
