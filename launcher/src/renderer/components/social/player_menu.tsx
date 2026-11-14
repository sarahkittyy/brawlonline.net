import type { RoomList } from "@accounts/types";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Divider from "@mui/material/Divider";
import ListItemText from "@mui/material/ListItemText";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import type { SocialPlayer } from "@social/types";
import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { RoomNameMessages } from "@/components/room_name/room_name.messages";
import { useAccount, useIsAdmin } from "@/lib/hooks/use_account";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useAdminRefresh } from "@/lib/social/admin_refresh";
import { activityOf } from "@/lib/social/friend_activity";
import { joinRoomByCode } from "@/lib/social/join_room";
import { useFriendActions, useFriendRelation } from "@/lib/social/use_friends";
import { inviteBlocked, useSocial, waitsForRoom } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";
import { useServices } from "@/services";
import type { AdminUser } from "@/services/backend/types";

import { AdminMessages } from "./admin.messages";
import styles from "./admin.module.css";
import { AdminRenameDialog } from "./admin_rename_dialog";
import { SocialMessages as Messages } from "./social.messages";

export type PlayerMenuTarget = { player: SocialPlayer; x: number; y: number };

/**
 * The right-click menu of a player's name (the online list and a chat tab's header): Message, View Profile, ...
 * Friends (docs/design/friends.md): a request from them first (Accept / Decline, then a line); Add friend or
 * Cancel friend request after Invite, and Join their room for a friend in a room the player can join;
 * Remove friend above Ignore user.
 * On an admin account, the admin tools after a line: Mute / Unmute, Rename (a box), Ban User (after "Are you sure?")
 * / Unban User. Not offered for another admin's account (the server refuses those too).
 */
export const PlayerMenu = ({
  target,
  onClose,
  showMessage = true,
}: {
  target: PlayerMenuTarget | null;
  onClose: () => void;
  showMessage?: boolean;
}) => {
  const room = useSocial((s) => s.state.room);
  const waiting = useSocial((s) => waitsForRoom(s.pendingRoom, s.state.room));
  const ignored = useSocial((s) => (target ? s.state.ignored.includes(target.player.uid) : false));
  const online = useSocial((s) => (target ? s.state.online.some((p) => p.uid === target.player.uid) : false));
  const { invite, setIgnored, message } = useSocialActions();
  const friendsLoaded = useSocial((s) => s.state.friends.loaded);
  const relation = useFriendRelation(target?.player.uid);
  const friendActions = useFriendActions();
  // Home's last room list, without polling it from wherever a name is (use_room_list.ts).
  const queryClient = useQueryClient();
  const uid = useAccount((s) => s.user?.uid);
  const rooms = target && relation === "friends" ? queryClient.getQueryData<RoomList>(["publicRooms", uid]) : undefined;
  const activity =
    target && rooms ? activityOf(target.player.uid, rooms.rooms, rooms.yourRoom?.code, undefined) : undefined;
  const joinable = activity?.kind === "room" && !activity.yours && activity.room.joinable ? activity.room : undefined;
  const openProfile = useProfileView((s) => s.open);
  const isAdmin = useIsAdmin();
  const blocked = waiting ? undefined : inviteBlocked(room);
  const player = target?.player;
  const [renaming, setRenaming] = useState<SocialPlayer | null>(null);
  const [banning, setBanning] = useState<SocialPlayer | null>(null);
  const admin = useAdminActions(isAdmin ? player : undefined);

  const act = (fn: (p: SocialPlayer) => unknown) => () => {
    onClose();
    if (player) {
      void fn(player);
    }
  };

  const u = admin.user;
  const adminItems =
    isAdmin && player
      ? [
          <Divider key="admin-line" />,
          ...(u?.admin
            ? []
            : [
                <MenuItem
                  key="mute"
                  onClick={act((p) => admin.setMuted(p, !u?.muted))}
                  disabled={u == null}
                  dense={true}
                  data-testid="admin-mute"
                >
                  <ListItemText>{u?.muted ? AdminMessages.unmute() : AdminMessages.mute()}</ListItemText>
                </MenuItem>,
              ]),
          <MenuItem key="rename" onClick={act(setRenaming)} dense={true} data-testid="admin-rename-item">
            <ListItemText>{AdminMessages.rename()}</ListItemText>
          </MenuItem>,
          ...(u?.admin
            ? []
            : [
                <MenuItem
                  key="ban"
                  onClick={act((p) => (u?.banned ? admin.setBanned(p, false) : setBanning(p)))}
                  disabled={u == null}
                  dense={true}
                  data-testid="admin-ban"
                >
                  <ListItemText>{u?.banned ? AdminMessages.unban() : AdminMessages.ban()}</ListItemText>
                </MenuItem>,
              ]),
        ]
      : [];

  return (
    <>
      <Menu
        open={target != null}
        onClose={onClose}
        anchorReference="anchorPosition"
        anchorPosition={target ? { top: target.y, left: target.x } : undefined}
        slotProps={{ paper: { sx: { minWidth: 180 } } }}
        onContextMenu={(e) => {
          e.preventDefault();
          onClose();
        }}
      >
        {friendsLoaded &&
          relation === "incoming" && [
            <MenuItem key="accept" onClick={act(friendActions.accept)} dense={true} data-testid="menu-accept-friend">
              <ListItemText>{Messages.acceptRequest()}</ListItemText>
            </MenuItem>,
            <MenuItem key="decline" onClick={act(friendActions.decline)} dense={true}>
              <ListItemText>{Messages.declineRequest()}</ListItemText>
            </MenuItem>,
            <Divider key="friend-line" />,
          ]}
        {showMessage && (
          <MenuItem onClick={act(message)} dense={true}>
            <ListItemText>{Messages.message()}</ListItemText>
          </MenuItem>
        )}
        <MenuItem onClick={act(openProfile)} dense={true}>
          <ListItemText>{Messages.viewProfile()}</ListItemText>
        </MenuItem>
        <MenuItem onClick={act(invite)} disabled={!online || blocked != null} dense={true}>
          <ListItemText
            primary={Messages.inviteToGame()}
            secondary={blocked === "full" ? Messages.roomFull() : undefined}
            secondaryTypographyProps={{ fontSize: 11 }}
          />
        </MenuItem>
        {friendsLoaded && relation === "none" && (
          <MenuItem onClick={act(friendActions.add)} dense={true} data-testid="menu-add-friend">
            <ListItemText>{Messages.addFriend()}</ListItemText>
          </MenuItem>
        )}
        {friendsLoaded && relation === "sent" && (
          <MenuItem onClick={act(friendActions.cancel)} dense={true}>
            <ListItemText>{Messages.cancelRequest()}</ListItemText>
          </MenuItem>
        )}
        {joinable && (
          <MenuItem onClick={act(() => joinRoomByCode(joinable.code))} dense={true}>
            <ListItemText
              primary={Messages.joinTheirRoom()}
              secondary={`${joinable.name ?? RoomNameMessages.defaultName(joinable.host)} · ${joinable.players}/${
                joinable.openSlots
              }`}
              secondaryTypographyProps={{ fontSize: 11 }}
            />
          </MenuItem>
        )}
        {friendsLoaded && relation === "friends" && (
          <MenuItem onClick={act(friendActions.remove)} dense={true} data-testid="menu-remove-friend">
            <ListItemText>{Messages.removeFriend()}</ListItemText>
          </MenuItem>
        )}
        <MenuItem onClick={act((p) => setIgnored(p, !ignored))} dense={true}>
          <ListItemText>{ignored ? Messages.unignoreUser() : Messages.ignoreUser()}</ListItemText>
        </MenuItem>
        {adminItems}
      </Menu>
      {isAdmin && (
        <>
          <AdminRenameDialog player={renaming} onClose={() => setRenaming(null)} />
          <BanQuestion
            player={banning}
            onClose={() => setBanning(null)}
            onBan={(p) => {
              setBanning(null);
              void admin.setBanned(p, true);
            }}
          />
        </>
      )}
    </>
  );
};

/** The small "Are you sure?" before a ban. */
const BanQuestion = ({
  player,
  onClose,
  onBan,
}: {
  player: SocialPlayer | null;
  onClose: () => void;
  onBan: (p: SocialPlayer) => void;
}) => (
  <Dialog open={player != null} onClose={onClose} maxWidth="xs" data-testid="admin-ban-question">
    <DialogTitle>{AdminMessages.banTitle()}</DialogTitle>
    <DialogContent>
      <p className={styles.banText}>{player ? AdminMessages.banText(player.displayName, player.connectCode) : ""}</p>
    </DialogContent>
    <DialogActions>
      <Button color="secondary" onClick={onClose} autoFocus={true}>
        {AdminMessages.cancel()}
      </Button>
      <Button color="error" onClick={() => player && onBan(player)} data-testid="admin-ban-confirm">
        {AdminMessages.banConfirm()}
      </Button>
    </DialogActions>
  </Dialog>
);

/** The menu's player as the admin tools see them (muted, banned, an admin), and the changes. */
function useAdminActions(player: SocialPlayer | undefined) {
  const { backendService } = useServices();
  const { showError, showSuccess } = useToasts();
  const bump = useAdminRefresh((s) => s.bump);
  const [user, setUser] = useState<AdminUser | null>(null);
  // Read each time the menu opens on a player (another admin, or pp-admin, may have changed them since).
  const uid = player?.uid;
  useEffect(() => {
    if (!uid) {
      return;
    }
    let alive = true;
    setUser(null);
    backendService
      .adminUser(uid)
      .then((u) => alive && setUser(u))
      .catch((err) => alive && showError(err));
    return () => {
      alive = false;
    };
  }, [uid, backendService, showError]);

  const done = (saved: AdminUser) => {
    setUser(saved);
    bump();
  };
  const setMuted = async (p: SocialPlayer, muted: boolean) => {
    try {
      done(await backendService.adminSetMuted(p.uid, muted));
      showSuccess(muted ? AdminMessages.muted(p.displayName) : AdminMessages.unmuted(p.displayName));
    } catch (err) {
      showError(err);
    }
  };
  const setBanned = async (p: SocialPlayer, banned: boolean) => {
    try {
      done(await backendService.adminSetBanned(p.uid, banned));
      showSuccess(banned ? AdminMessages.banned(p.displayName) : AdminMessages.unbanned(p.displayName));
    } catch (err) {
      showError(err);
    }
  };
  return { user: player && user?.uid === player.uid ? user : undefined, setMuted, setBanned };
}
