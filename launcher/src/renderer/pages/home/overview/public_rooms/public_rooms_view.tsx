import AddRoundedIcon from "@mui/icons-material/AddRounded";
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
  /** Home's "Create room": the game creates a room as WITH FRIENDS > Create Room does. */
  onCreate?: () => void;
  /** A create is being handed to the game. */
  creating?: boolean;
  /** Shown left of Create room: the ruleset box (ruleset_box.tsx). */
  rulesetBox?: React.ReactNode;
  /** The player has never created a room from here: "Create room" glows until they do. */
  highlightCreate?: boolean;
  /** A click on a room's ruleset name: shows the ruleset. */
  onShowRuleset?: (room: PublicRoom) => void;
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
  onCreate,
  creating = false,
  rulesetBox,
  highlightCreate = false,
  onShowRuleset,
  forceRowState,
}: PublicRoomsViewProps) => {
  const online =
    state.kind === "ready" ? (
      <div className={styles.online} data-testid="players-online">
        <span className={styles.onlineDot} />
        {Messages.playersInGame(state.online)}
      </div>
    ) : undefined;

  return (
    <div className={styles.block}>
      <ContentBlock
        fill={true}
        title={Messages.publicRooms()}
        titleAction={
          onCreate && state.kind !== "logged-out" ? (
            <div className={styles.titleActions}>
              {rulesetBox}
              <Tooltip title={Messages.createRoomTooltip()} placement="top" enterDelay={400} disableInteractive={true}>
                <span className={styles.createRoom}>
                  <Button
                    variant="contained"
                    color="secondary"
                    size="small"
                    disableElevation={true}
                    disabled={creating || joiningCode != null}
                    startIcon={creating ? <CircularProgress color="inherit" size={14} /> : <AddRoundedIcon />}
                    onClick={onCreate}
                    className={clsx(highlightCreate && !creating && styles.createHighlight)}
                    data-testid="create-room"
                    sx={{ textTransform: "none", fontWeight: 600, borderRadius: "8px", px: 1.5, whiteSpace: "nowrap" }}
                  >
                    {Messages.createRoom()}
                  </Button>
                </span>
              </Tooltip>
            </div>
          ) : undefined
        }
        endIcon={online}
        content={renderBody()}
      />
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
                {onCreate ? (
                  <p>{Messages.createAbove()}</p>
                ) : (
                  <p>
                    {Messages.createInGame()} <span className={styles.menuPath}>{Messages.createInGamePath()}</span>
                  </p>
                )}
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
                  onShowRuleset={onShowRuleset}
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
  onShowRuleset,
  forceState,
}: {
  room: PublicRoom;
  joining: boolean;
  /** Another join is in progress. */
  busy: boolean;
  onJoin: (room: PublicRoom) => void;
  onShowRuleset?: (room: PublicRoom) => void;
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
        <RulesetCell room={room} onShow={onShowRuleset} />
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

/**
 * The room's ruleset, left of the mode: a published ruleset's name (a click shows it), "Custom (based on ...)" for
 * custom rules, nothing for P+ default. Long names are cut, the whole name in the tooltip. Inside the row's button, so
 * it stops the click (and Enter / Space) from joining the room.
 */
const RulesetCell = ({ room, onShow }: { room: PublicRoom; onShow?: (room: PublicRoom) => void }) => {
  const r = room.ruleset;
  if (!r || (!r.code && !r.basedOn)) {
    return <span className={styles.ruleset} />;
  }
  if (!r.code) {
    const text = Messages.customRuleset(r.basedOn ?? "");
    return (
      <Tooltip title={text} placement="top" enterDelay={400} disableInteractive={true}>
        <span className={clsx(styles.ruleset, styles.rulesetCustom)} data-ruleset-cell="custom">
          {text}
        </span>
      </Tooltip>
    );
  }
  const full = Messages.rulesetTooltip(r.name, r.version, r.by);
  const show = (e: React.SyntheticEvent) => {
    e.stopPropagation();
    e.preventDefault();
    onShow?.(room);
  };
  return (
    <Tooltip title={full} placement="top" enterDelay={400} disableInteractive={true}>
      <span
        className={clsx(styles.ruleset, onShow && styles.rulesetLink)}
        role={onShow ? "button" : undefined}
        tabIndex={onShow ? 0 : undefined}
        data-ruleset-cell={r.code}
        onClick={onShow ? show : undefined}
        onKeyDown={
          onShow
            ? (e) => {
                if (e.key === "Enter" || e.key === " ") {
                  show(e);
                }
              }
            : undefined
        }
      >
        {r.name}
      </span>
    </Tooltip>
  );
};
