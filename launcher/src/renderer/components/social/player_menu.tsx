import ListItemText from "@mui/material/ListItemText";
import Menu from "@mui/material/Menu";
import MenuItem from "@mui/material/MenuItem";
import type { SocialPlayer } from "@social/types";

import { inviteBlocked, useSocial, waitsForRoom } from "@/lib/social/use_social";
import { useSocialActions } from "@/lib/social/use_social_actions";

import { SocialMessages as Messages } from "./social.messages";

export type PlayerMenuTarget = { player: SocialPlayer; x: number; y: number };

/** The right-click menu of a player's name (the online list and a chat tab's header). */
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
  const blocked = waiting ? undefined : inviteBlocked(room);
  const player = target?.player;

  const act = (fn: (p: SocialPlayer) => unknown) => () => {
    onClose();
    if (player) {
      void fn(player);
    }
  };

  return (
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
      {showMessage && (
        <MenuItem onClick={act(message)} dense={true}>
          <ListItemText>{Messages.message()}</ListItemText>
        </MenuItem>
      )}
      <MenuItem onClick={act(invite)} disabled={!online || blocked != null} dense={true}>
        <ListItemText
          primary={Messages.inviteToGame()}
          secondary={blocked === "full" ? Messages.roomFull() : undefined}
          secondaryTypographyProps={{ fontSize: 11 }}
        />
      </MenuItem>
      <MenuItem onClick={act((p) => setIgnored(p, !ignored))} dense={true}>
        <ListItemText>{ignored ? Messages.unignoreUser() : Messages.ignoreUser()}</ListItemText>
      </MenuItem>
    </Menu>
  );
};
