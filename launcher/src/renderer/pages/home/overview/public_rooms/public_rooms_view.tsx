import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import CloudOffOutlinedIcon from "@mui/icons-material/CloudOffOutlined";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import MeetingRoomOutlinedIcon from "@mui/icons-material/MeetingRoomOutlined";
import PersonOutlineIcon from "@mui/icons-material/PersonOutline";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import React from "react";

import type { PublicRoom, RoomMode } from "@/services/backend/types";

import { ContentBlock } from "../content_block/content_block";
import { PublicRoomsMessages as Messages } from "./public_rooms.messages";
import styles from "./public_rooms.module.css";

/** What the block shows. */
export type PublicRoomsState =
  | { kind: "logged-out" }
  | { kind: "loading" }
  | { kind: "unreachable" }
  | { kind: "ready"; online: number; rooms: PublicRoom[] };

export type PublicRoomsViewProps = {
  state: PublicRoomsState;
  /** The room whose join is in progress (its row shows a spinner, the others wait). */
  joiningCode?: string;
  onJoin: (room: PublicRoom) => void;
  onRetry: () => void;
  onLogIn: () => void;
  /** Stories and screenshots: show this row as hovered or pressed. */
  forceRowState?: { code: string; state: "hover" | "pressed" };
};

/**
 * Home > Overview's room list: players online and the public rooms (docs/design/rooms.md §1.1).
 * A full room is shown but cannot be clicked; a room in game can be clicked while an open slot is
 * empty (the player waits in the room until the game ends).
 */
export const PublicRoomsView = ({
  state,
  joiningCode,
  onJoin,
  onRetry,
  onLogIn,
  forceRowState,
}: PublicRoomsViewProps) => {
  const online =
    state.kind === "ready" ? (
      <div className={styles.online} data-testid="players-online">
        <span className={styles.onlineDot} />
        {Messages.playersOnline(state.online)}
      </div>
    ) : undefined;

  return (
    <div className={styles.block}>
      <ContentBlock fill={true} title={Messages.publicRooms()} endIcon={online} content={renderBody()} />
    </div>
  );

  function renderBody() {
    switch (state.kind) {
      case "logged-out":
        return (
          <div className={styles.centered}>
            <LockOutlinedIcon style={{ fontSize: 48 }} />
            <p>{Messages.loggedOut()}</p>
            <Button color="secondary" variant="outlined" onClick={onLogIn}>
              {Messages.logIn()}
            </Button>
          </div>
        );
      case "loading":
        return (
          <div className={styles.centered}>
            <CircularProgress color="inherit" />
          </div>
        );
      case "unreachable":
        return (
          <div className={styles.centered}>
            <CloudOffOutlinedIcon style={{ fontSize: 48 }} />
            <p>{Messages.unreachable()}</p>
            <Button color="secondary" onClick={onRetry}>
              {Messages.retry()}
            </Button>
          </div>
        );
      case "ready":
        if (state.rooms.length === 0) {
          return (
            <div className={styles.centered}>
              <MeetingRoomOutlinedIcon style={{ fontSize: 48 }} />
              <div>
                <p>{Messages.noRooms()}</p>
                <p>
                  {Messages.createInGame()} <span className={styles.menuPath}>{Messages.createInGamePath()}</span>
                </p>
              </div>
            </div>
          );
        }
        return (
          <ul className={styles.list}>
            {state.rooms.map((room) => (
              <li key={room.code}>
                <RoomRow
                  room={room}
                  joining={joiningCode === room.code}
                  busy={joiningCode != null}
                  onJoin={onJoin}
                  forceState={forceRowState?.code === room.code ? forceRowState.state : undefined}
                />
              </li>
            ))}
          </ul>
        );
    }
  }
};

const modeLabel = (mode: RoomMode): string => {
  switch (mode) {
    case "1v1":
      return Messages.modeOneVsOne();
    case "ffa":
      return Messages.modeFfa();
    case "teams":
      return Messages.modeTeams();
  }
};

export const RoomRow = React.memo(function RoomRow({
  room,
  joining,
  busy,
  onJoin,
  forceState,
}: {
  room: PublicRoom;
  joining: boolean;
  /** Another join is in progress. */
  busy: boolean;
  onJoin: (room: PublicRoom) => void;
  forceState?: "hover" | "pressed";
}) {
  const inGame = room.status === "in-game";
  const disabled = !room.joinable;
  const tooltip = disabled ? Messages.fullTooltip() : inGame ? Messages.joinInGameTooltip() : Messages.joinTooltip();

  return (
    <Tooltip title={tooltip} placement="top" enterDelay={600} disableInteractive={true}>
      <button
        type="button"
        className={clsx(
          styles.row,
          disabled && styles.disabled,
          forceState === "hover" && styles.hover,
          forceState === "pressed" && styles.pressed,
        )}
        aria-disabled={disabled || busy}
        data-room={room.code}
        onClick={() => {
          if (!disabled && !busy) {
            onJoin(room);
          }
        }}
      >
        <div className={styles.main}>
          <div className={styles.host}>{room.host}</div>
          <div className={styles.names}>{room.names.join(" · ")}</div>
        </div>
        <span className={styles.mode}>{modeLabel(room.mode)}</span>
        <span className={styles.players}>
          <PersonOutlineIcon style={{ fontSize: 16 }} />
          {Messages.playersInRoom(room.players, room.openSlots)}
        </span>
        <span className={clsx(styles.status, inGame ? styles.inGame : styles.waiting)}>
          <span className={styles.statusDot} />
          {inGame ? Messages.statusInGame() : Messages.statusWaiting()}
        </span>
        <span className={styles.action}>
          {joining ? (
            <CircularProgress color="inherit" size={14} />
          ) : disabled ? (
            Messages.full()
          ) : (
            <>
              {Messages.join()}
              <ChevronRightIcon style={{ fontSize: 18 }} />
            </>
          )}
        </span>
      </button>
    </Tooltip>
  );
});
