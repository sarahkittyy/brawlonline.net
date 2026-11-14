import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import type { Friend, Invite } from "@social/types";
import { clsx } from "clsx";
import React from "react";

import { useToasts } from "@/lib/hooks/use_toasts";
import { useFriendActions } from "@/lib/social/use_friends";
import { useHandOff } from "@/lib/social/use_hand_off";
import { useSocial } from "@/lib/social/use_social";

import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

const modeLabel = { "1v1": "1v1", ffa: "FFA", teams: "Teams" } as const;

/**
 * "bob invited you. Join?" in the bottom right, one per inviter, newest at the bottom; and the
 * friend requests that arrived while the launcher was open (docs/design/friends.md), which stay
 * until answered or closed (a closed one stays in Friends > Requests).
 */
export const InviteNotifications = React.memo(function InviteNotifications() {
  const invites = useSocial((s) => s.state.invites);
  const requests = useSocial((s) => s.state.requestPopups);
  if (invites.length === 0 && requests.length === 0) {
    return null;
  }
  return (
    <div className={styles.invites}>
      {requests.slice(-3).map((player) => (
        <RequestCard key={player.uid} player={player} />
      ))}
      {invites.slice(-4).map((invite) => (
        <InviteCard key={invite.id} invite={invite} />
      ))}
    </div>
  );
});

const RequestCard = ({ player }: { player: Friend }) => {
  const { accept, decline } = useFriendActions();
  return (
    <div
      className={clsx(styles.invite, styles.request)}
      role="alertdialog"
      data-testid={`friend-request-${player.uid}`}
    >
      <div>{Messages.sentYouRequest(player.displayName)}</div>
      <div className={styles.inviteDetail}>{player.connectCode}</div>
      <div className={styles.inviteActions}>
        <button type="button" className={styles.yes} onClick={() => void accept(player)}>
          {Messages.accept()}
        </button>
        <button type="button" className={styles.no} onClick={() => void decline(player)}>
          {Messages.decline()}
        </button>
      </div>
      <button
        type="button"
        className={clsx(styles.iconButton, styles.inviteClose)}
        aria-label={Messages.dismiss()}
        onClick={() => void window.electron.social.dismissRequestPopup(player.uid)}
      >
        <CloseRoundedIcon style={{ fontSize: 15 }} />
      </button>
    </div>
  );
};

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
      <div>
        {invite.kind !== "room"
          ? Messages.invitedYouDirect(name)
          : invite.room?.twos
          ? Messages.invitedYouTwos(name)
          : Messages.invitedYouRoom(name)}
      </div>
      <div className={styles.inviteDetail}>
        {invite.room
          ? `${invite.room.twos ? "2v2" : modeLabel[invite.room.mode]} · ${invite.room.players}/${
              invite.room.openSlots
            }`
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
