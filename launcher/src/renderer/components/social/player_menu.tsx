import type { RoomList } from "@accounts/types";
import BlockRoundedIcon from "@mui/icons-material/BlockRounded";
import CampaignOutlinedIcon from "@mui/icons-material/CampaignOutlined";
import ChatBubbleOutlineRoundedIcon from "@mui/icons-material/ChatBubbleOutlineRounded";
import CheckRoundedIcon from "@mui/icons-material/CheckRounded";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import GavelRoundedIcon from "@mui/icons-material/GavelRounded";
import LoginRoundedIcon from "@mui/icons-material/LoginRounded";
import MailOutlineRoundedIcon from "@mui/icons-material/MailOutlineRounded";
import PersonOutlineRoundedIcon from "@mui/icons-material/PersonOutlineRounded";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import Divider from "@mui/material/Divider";
import ListItemIcon from "@mui/material/ListItemIcon";
import ListItemText from "@mui/material/ListItemText";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import { createSvgIcon } from "@mui/material/utils";
import type { SocialPlayer } from "@social/types";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";

import { RoomNameMessages } from "@/components/room_name/room_name.messages";
import { useAccount, useIsAdmin } from "@/lib/hooks/use_account";
import { useProfileView } from "@/lib/hooks/use_profile_view";
import { useToasts } from "@/lib/hooks/use_toasts";
import { useAdminRefresh } from "@/lib/social/admin_refresh";
import { activityOf } from "@/lib/social/friend_activity";
import { joinRoomByCode } from "@/lib/social/join_room";
import { useFriendActions, useFriendRelation } from "@/lib/social/use_friends";
import { usePlayerMenu } from "@/lib/social/use_player_menu";
import { inviteBlocked, useSocial, waitsForRoom } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";
import { useServices } from "@/services";
import type { AdminUser } from "@/services/backend/types";

import { AdminMessages } from "./admin.messages";
import styles from "./admin.module.css";
import { AdminRenameDialog } from "./admin_rename_dialog";
import { AddFriendIcon, PendingFriendIcon, RemoveFriendIcon } from "./friend_icons";
import { SocialMessages as Messages } from "./social.messages";

export type PlayerMenuTarget = { player: SocialPlayer; x: number; y: number };

/** Each item's icon, left of its text (Sarah's board, 2026-11-14). */
const ICON = { fontSize: "small" } as const;
const ICON_SLOT = { minWidth: "30px !important" };

/** CampaignOutlined's megaphone with a line through it: Mute. */
const MutedIcon = createSvgIcon(
  [
    <defs key="defs">
      <mask id="pp-muted-megaphone">
        <rect width="24" height="24" fill="white" />
        <path d="M2.6 1.2 23.4 22" stroke="black" strokeWidth="4.4" />
      </mask>
    </defs>,
    <path
      key="megaphone"
      mask="url(#pp-muted-megaphone)"
      d="M18 11v2h4v-2zm-2 6.61c.96.71 2.21 1.65 3.2 2.39.4-.53.8-1.07 1.2-1.6-.99-.74-2.24-1.68-3.2-2.4-.4.54-.8 1.08-1.2 1.61M20.4 5.6c-.4-.53-.8-1.07-1.2-1.6-.99.74-2.24 1.68-3.2 2.4.4.53.8 1.07 1.2 1.6.96-.72 2.21-1.65 3.2-2.4M4 9c-1.1 0-2 .9-2 2v2c0 1.1.9 2 2 2h1v4h2v-4h1l5 3V6L8 9zm5.03 1.71L11 9.53v4.94l-1.97-1.18-.48-.29H4v-2h4.55zM15.5 12c0-1.33-.58-2.53-1.5-3.35v6.69c.92-.81 1.5-2.01 1.5-3.34"
    />,
    <path key="line" d="M3.4 2 22.6 21.2" stroke="currentColor" strokeWidth="2" strokeLinecap="round" fill="none" />,
  ],
  "Muted",
);

/**
 * The menu of every name that has a hover card, opened by usePlayerMenuOpener (lib/social/use_player_menu.ts).
 * A list that only had the name gets the connect code from the player's profile (the hover card's, usually
 * already fetched).
 */
export const PlayerMenuHost = () => {
  const request = usePlayerMenu((s) => s.request);
  const close = usePlayerMenu((s) => s.close);
  const { backendService } = useServices();
  const uid = request?.player.uid;
  const needCode = request != null && !request.player.connectCode;
  const { data: profile } = useQuery({
    queryKey: ["profile", uid],
    queryFn: () => backendService.fetchProfile(uid!),
    staleTime: 5 * 60_000,
    enabled: needCode,
  });
  const target: PlayerMenuTarget | null = request
    ? {
        player: {
          uid: request.player.uid,
          displayName: request.player.displayName,
          connectCode: request.player.connectCode || (profile?.uid === uid ? profile?.connectCode : undefined) || "",
          ...(request.player.country ? { country: request.player.country } : {}),
        },
        x: request.x,
        y: request.y,
      }
    : null;
  return <PlayerMenu target={target} onClose={close} showMessage={request?.showMessage ?? true} />;
};

/**
 * The right-click menu of a player's name (the online list and a chat tab's header, and through PlayerMenuHost every
 * name with a hover card): Message, View Profile, ..., each with its icon.
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
                  <ListItemIcon sx={ICON_SLOT}>
                    {u?.muted ? <CampaignOutlinedIcon {...ICON} /> : <MutedIcon {...ICON} />}
                  </ListItemIcon>
                  <ListItemText>{u?.muted ? AdminMessages.unmute() : AdminMessages.mute()}</ListItemText>
                </MenuItem>,
              ]),
          <MenuItem key="rename" onClick={act(setRenaming)} dense={true} data-testid="admin-rename-item">
            <ListItemIcon sx={ICON_SLOT}>
              <EditOutlinedIcon {...ICON} />
            </ListItemIcon>
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
                  <ListItemIcon sx={ICON_SLOT}>
                    <GavelRoundedIcon {...ICON} />
                  </ListItemIcon>
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
              <ListItemIcon sx={ICON_SLOT}>
                <CheckRoundedIcon {...ICON} />
              </ListItemIcon>
              <ListItemText>{Messages.acceptRequest()}</ListItemText>
            </MenuItem>,
            <MenuItem key="decline" onClick={act(friendActions.decline)} dense={true}>
              <ListItemIcon sx={ICON_SLOT}>
                <CloseRoundedIcon {...ICON} />
              </ListItemIcon>
              <ListItemText>{Messages.declineRequest()}</ListItemText>
            </MenuItem>,
            <Divider key="friend-line" />,
          ]}
        {showMessage && (
          <MenuItem onClick={act(message)} dense={true}>
            <ListItemIcon sx={ICON_SLOT}>
              <ChatBubbleOutlineRoundedIcon {...ICON} />
            </ListItemIcon>
            <ListItemText>{Messages.message()}</ListItemText>
          </MenuItem>
        )}
        <MenuItem onClick={act(openProfile)} dense={true}>
          <ListItemIcon sx={ICON_SLOT}>
            <PersonOutlineRoundedIcon {...ICON} />
          </ListItemIcon>
          <ListItemText>{Messages.viewProfile()}</ListItemText>
        </MenuItem>
        <MenuItem onClick={act(invite)} disabled={!online || blocked != null} dense={true}>
          <ListItemIcon sx={ICON_SLOT}>
            <MailOutlineRoundedIcon {...ICON} />
          </ListItemIcon>
          <ListItemText
            primary={Messages.inviteToGame()}
            secondary={blocked === "full" ? Messages.roomFull() : undefined}
            secondaryTypographyProps={{ fontSize: 11 }}
          />
        </MenuItem>
        {friendsLoaded && relation === "none" && (
          <MenuItem onClick={act(friendActions.add)} dense={true} data-testid="menu-add-friend">
            <ListItemIcon sx={ICON_SLOT}>
              <AddFriendIcon {...ICON} />
            </ListItemIcon>
            <ListItemText>{Messages.addFriend()}</ListItemText>
          </MenuItem>
        )}
        {friendsLoaded && relation === "sent" && (
          <MenuItem onClick={act(friendActions.cancel)} dense={true}>
            <ListItemIcon sx={ICON_SLOT}>
              <PendingFriendIcon {...ICON} />
            </ListItemIcon>
            <ListItemText>{Messages.cancelRequest()}</ListItemText>
          </MenuItem>
        )}
        {joinable && (
          <MenuItem onClick={act(() => joinRoomByCode(joinable.code))} dense={true}>
            <ListItemIcon sx={ICON_SLOT}>
              <LoginRoundedIcon {...ICON} />
            </ListItemIcon>
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
            <ListItemIcon sx={ICON_SLOT}>
              <RemoveFriendIcon {...ICON} />
            </ListItemIcon>
            <ListItemText>{Messages.removeFriend()}</ListItemText>
          </MenuItem>
        )}
        <MenuItem onClick={act((p) => setIgnored(p, !ignored))} dense={true}>
          <ListItemIcon sx={ICON_SLOT}>
            <BlockRoundedIcon {...ICON} />
          </ListItemIcon>
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
