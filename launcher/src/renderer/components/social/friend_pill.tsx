import CheckRoundedIcon from "@mui/icons-material/CheckRounded";
import ListItemIcon from "@mui/material/ListItemIcon";
import ListItemText from "@mui/material/ListItemText";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import Tooltip from "@mui/material/Tooltip";
import type { SocialPlayer } from "@social/types";
import { clsx } from "clsx";
import React, { useState } from "react";

import { useFriendActions, useFriendRelation } from "@/lib/social/use_friends";
import { useSocial } from "@/lib/social/use_social";

import { AddFriendIcon, FriendsIcon, PendingFriendIcon, RemoveFriendIcon } from "./friend_icons";
import { SocialMessages as Messages } from "./social.messages";
import styles from "./social.module.css";

/**
 * The small friend button beside a name (docs/design/friends.md): a chat window's title and a
 * profile. Not friends: "+ Add"; asked: "Sent" (a click offers to take it back); asked by them:
 * "Accept"; friends: the friends icon (a click offers Remove friend). `long`: the profile's
 * wording ("Add Friend", "Request Sent", "Friends"). Nothing until the lists are read, and nothing
 * on the player's own name.
 */
export const FriendPill = ({ player, long = false }: { player: SocialPlayer; long?: boolean }) => {
  const loaded = useSocial((s) => s.state.friends.loaded);
  const relation = useFriendRelation(player.uid);
  const { add, accept, cancel, remove } = useFriendActions();
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const [busy, setBusy] = useState(false);
  if (!loaded || relation === "self") {
    return null;
  }

  const run = (work: () => Promise<unknown>) => {
    setAnchor(null);
    setBusy(true);
    void work().finally(() => setBusy(false));
  };
  const stop = (e: React.SyntheticEvent) => e.stopPropagation();

  let pill: React.ReactElement;
  switch (relation) {
    case "none":
      pill = (
        <button
          type="button"
          className={clsx(styles.friendPill, styles.friendAdd)}
          disabled={busy}
          aria-label={Messages.addFriend()}
          data-testid="friend-pill-add"
          onClick={(e) => {
            stop(e);
            run(() => add(player));
          }}
        >
          <AddFriendIcon style={{ fontSize: 14 }} />
          {long ? Messages.addFriendLong() : Messages.addFriendShort()}
        </button>
      );
      break;
    case "sent":
      pill = (
        <Tooltip title={Messages.requestSentTooltip()} placement="top" disableInteractive={true}>
          <button
            type="button"
            className={clsx(styles.friendPill, styles.friendSent)}
            disabled={busy}
            data-testid="friend-pill-sent"
            onClick={(e) => {
              stop(e);
              setAnchor(e.currentTarget);
            }}
          >
            <PendingFriendIcon style={{ fontSize: 14 }} />
            {Messages.requestSentShort()}
          </button>
        </Tooltip>
      );
      break;
    case "incoming":
      pill = (
        <Tooltip title={Messages.acceptTooltip()} placement="top" disableInteractive={true}>
          <button
            type="button"
            className={clsx(styles.friendPill, styles.friendAccept)}
            disabled={busy}
            data-testid="friend-pill-accept"
            onClick={(e) => {
              stop(e);
              run(() => accept(player));
            }}
          >
            <CheckRoundedIcon style={{ fontSize: 14 }} />
            {Messages.acceptShort()}
          </button>
        </Tooltip>
      );
      break;
    case "friends":
      pill = (
        <Tooltip title={Messages.youAreFriends()} placement="top" disableInteractive={true}>
          <button
            type="button"
            className={clsx(styles.friendPill, styles.friendYes)}
            aria-label={Messages.youAreFriends()}
            data-testid="friend-pill-friends"
            onClick={(e) => {
              stop(e);
              setAnchor(e.currentTarget);
            }}
          >
            <FriendsIcon style={{ fontSize: 16 }} />
            {long && Messages.friendsLong()}
          </button>
        </Tooltip>
      );
      break;
  }

  return (
    <>
      {pill}
      <Menu
        open={anchor != null}
        anchorEl={anchor}
        onClose={() => setAnchor(null)}
        onClick={stop}
        slotProps={{ paper: { sx: { minWidth: 180 } } }}
      >
        {relation === "sent" && (
          <MenuItem dense={true} onClick={() => run(() => cancel(player))}>
            <ListItemIcon>
              <RemoveFriendIcon fontSize="small" />
            </ListItemIcon>
            <ListItemText>{Messages.cancelRequest()}</ListItemText>
          </MenuItem>
        )}
        {relation === "friends" && (
          <MenuItem dense={true} onClick={() => run(() => remove(player))} data-testid="friend-pill-remove">
            <ListItemIcon>
              <RemoveFriendIcon fontSize="small" />
            </ListItemIcon>
            <ListItemText>{Messages.removeFriend()}</ListItemText>
          </MenuItem>
        )}
      </Menu>
    </>
  );
};
