import type { PublicModRoom } from "@accounts/types";
import type {
  ModBuild,
  ModRoomRequest,
  ModRoomSlot,
  ModRoomStatus,
  ModsController,
  ModsState,
} from "@dolphin/mods/types";
import { controllerOf } from "@dolphin/mods/types";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import ContentCopyRoundedIcon from "@mui/icons-material/ContentCopyRounded";
import CreateNewFolderOutlinedIcon from "@mui/icons-material/CreateNewFolderOutlined";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import SportsEsportsOutlinedIcon from "@mui/icons-material/SportsEsportsOutlined";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import type { ReactNode } from "react";
import { useState } from "react";

import { ContentBlock } from "../overview/content_block/content_block";
import { ModsMessages as Messages } from "./mods.messages";
import styles from "./mods.module.css";

/** The public Brawl Mods rooms (Home's room list). */
export type ModsPublicRooms =
  | { kind: "logged-out" }
  | { kind: "loading" }
  | { kind: "unreachable" }
  | { kind: "ready"; rooms: PublicModRoom[] };

export type ModsViewProps = {
  /** null while it loads. */
  state: ModsState | null;
  publicRooms: ModsPublicRooms;
  onLogIn: () => void;
  /** Hashing progress (0-1) by build id. */
  hashProgress: Record<string, number>;
  /** A build whose host or join is starting Dolphin. */
  startingBuildId: string | null;
  error: string | null;
  onControllerChange: (build: ModBuild, controller: ModsController) => void;
  onAddBuild: () => void;
  onAddFolder: () => void;
  onConfigureControllers: () => void;
  onRemoveBuild: (build: ModBuild) => void;
  onHost: (build: ModBuild, isPublic: boolean) => void;
  onJoin: (build: ModBuild, code: string) => void;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
};

const fileName = (p: string | null) => (p ? p.split(/[\\/]/).pop() ?? p : null);

/** Home > Other Games (docs/design/brawl-mods.md §8): the player's games and mods, and the room Dolphin runs. */
export const ModsView = (props: ModsViewProps) => {
  const { state, error } = props;
  const status = state?.running ? state.status : null;
  return (
    <div className={styles.page}>
      <p className={styles.intro}>{Messages.intro()}</p>
      {error && <div className={styles.error}>{error}</div>}
      {state?.running && <RoomPanel status={status} onRequest={props.onRequest} onCopyCode={props.onCopyCode} />}
      <ContentBlock
        title={Messages.yourBuilds()}
        titleAction={
          <div className={styles.roomActions}>
            <Tooltip title={Messages.addBuildTooltip()}>
              <Button size="small" startIcon={<AddRoundedIcon />} onClick={props.onAddBuild}>
                {Messages.addBuild()}
              </Button>
            </Tooltip>
            <Tooltip title={Messages.addFolderTooltip()}>
              <Button size="small" startIcon={<CreateNewFolderOutlinedIcon />} onClick={props.onAddFolder}>
                {Messages.addFolder()}
              </Button>
            </Tooltip>
            <Tooltip title={Messages.controllerSetupTooltip()}>
              <span>
                <Button
                  size="small"
                  startIcon={<SportsEsportsOutlinedIcon />}
                  disabled={!!state?.running}
                  onClick={props.onConfigureControllers}
                >
                  {Messages.controllerSetup()}
                </Button>
              </span>
            </Tooltip>
          </div>
        }
        content={
          <div className={styles.builds}>
            {state && state.builds.length === 0 && <p className={styles.empty}>{Messages.noBuilds()}</p>}
            {state?.builds.map((build) => (
              <BuildRow
                key={build.id}
                build={build}
                progress={props.hashProgress[build.id]}
                starting={props.startingBuildId === build.id}
                disabled={!!state.running || props.startingBuildId !== null}
                onRemove={() => props.onRemoveBuild(build)}
                onControllerChange={(c) => props.onControllerChange(build, c)}
                onHost={(isPublic) => props.onHost(build, isPublic)}
                onJoin={(code) => props.onJoin(build, code)}
              />
            ))}
          </div>
        }
      />
      <PublicRoomsBlock
        rooms={props.publicRooms}
        builds={state?.builds ?? []}
        disabled={!!state?.running || props.startingBuildId !== null}
        onJoin={props.onJoin}
        onLogIn={props.onLogIn}
      />
    </div>
  );
};

const PublicRoomsBlock = ({
  rooms,
  builds,
  disabled,
  onJoin,
  onLogIn,
}: {
  rooms: ModsPublicRooms;
  builds: ModBuild[];
  disabled: boolean;
  onJoin: (build: ModBuild, code: string) => void;
  onLogIn: () => void;
}) => {
  let content: ReactNode;
  if (rooms.kind === "logged-out") {
    content = (
      <div className={styles.centered}>
        <p>{Messages.publicRoomsLoggedOut()}</p>
        <Button variant="outlined" size="small" onClick={onLogIn}>
          {Messages.logIn()}
        </Button>
      </div>
    );
  } else if (rooms.kind === "loading") {
    content = <p className={styles.empty}>{Messages.publicRoomsLoading()}</p>;
  } else if (rooms.kind === "unreachable") {
    content = <p className={styles.empty}>{Messages.publicRoomsUnreachable()}</p>;
  } else if (rooms.rooms.length === 0) {
    content = <p className={styles.empty}>{Messages.publicRoomsNone()}</p>;
  } else {
    content = (
      <div className={styles.slots}>
        {rooms.rooms.map((room) => {
          const build = builds.find((b) => b.hash === room.build.hash);
          const label = Messages.buildName(room.build.name, room.build.hash.slice(0, 8));
          return (
            <div key={room.code} className={clsx(styles.slot, !build && styles.closed)}>
              <span className={styles.player}>
                <span className={styles.name}>{Messages.roomHost(room.hostName)}</span>
                <span className={styles.code}>{label}</span>
                <span className={styles.badge}>{Messages.roomPlayers(room.players, room.open)}</span>
              </span>
              <Tooltip
                title={
                  build
                    ? Messages.joinPublicTooltip(build.name)
                    : Messages.needBuild(room.build.name, room.build.hash.slice(0, 8))
                }
              >
                <span>
                  <Button
                    size="small"
                    variant="outlined"
                    disabled={!build || disabled}
                    onClick={() => build && onJoin(build, room.code)}
                  >
                    {Messages.join()}
                  </Button>
                </span>
              </Tooltip>
            </div>
          );
        })}
      </div>
    );
  }
  return <ContentBlock title={Messages.publicRooms()} content={<div className={styles.room}>{content}</div>} />;
};

const BuildRow = ({
  build,
  progress,
  starting,
  disabled,
  onRemove,
  onControllerChange,
  onHost,
  onJoin,
}: {
  build: ModBuild;
  progress: number | undefined;
  starting: boolean;
  disabled: boolean;
  onRemove: () => void;
  onControllerChange: (controller: ModsController) => void;
  onHost: (isPublic: boolean) => void;
  onJoin: (code: string) => void;
}) => {
  const [code, setCode] = useState("");
  const [isPublic, setIsPublic] = useState(true);
  const hashing = progress !== undefined;
  return (
    <div className={styles.build}>
      <div className={styles.buildHead}>
        <div className={styles.buildName}>
          <span className={styles.name}>{build.name}</span>
          {build.hash ? (
            <Tooltip title={Messages.hashTooltip(build.hash)}>
              <span className={styles.hash}>{build.hash.slice(0, 8)}</span>
            </Tooltip>
          ) : (
            <span className={styles.hashMissing}>{Messages.notHashed()}</span>
          )}
        </div>
        <div className={styles.parts}>
          {build.gameId && <span className={styles.gameId}>{build.gameId}</span>}
          {Messages.parts(fileName(build.discPath)!, fileName(build.launcherPath), fileName(build.sdPath))}
        </div>
        <Tooltip title={Messages.removeTooltip()}>
          <span>
            <IconButton size="small" aria-label={Messages.remove()} onClick={onRemove} disabled={disabled || hashing}>
              <DeleteOutlineRoundedIcon fontSize="small" />
            </IconButton>
          </span>
        </Tooltip>
      </div>
      {hashing && (
        <div className={styles.progress}>
          <LinearProgress variant="determinate" value={progress * 100} />
          <span>{Messages.checking(Math.floor(progress * 100))}</span>
        </div>
      )}
      <div className={styles.actions}>
        <Tooltip title={Messages.controllerTooltip()}>
          <Select
            size="small"
            value={controllerOf(build)}
            disabled={disabled}
            inputProps={{ "aria-label": Messages.controller() }}
            onChange={(e) => onControllerChange(e.target.value as ModsController)}
          >
            <MenuItem value="gc">{Messages.gameCube()}</MenuItem>
            <MenuItem value="wiimote">{Messages.wiiRemote()}</MenuItem>
          </Select>
        </Tooltip>
        <Tooltip title={Messages.publicRoomTooltip()}>
          <label className={styles.toggle}>
            <Switch size="small" checked={isPublic} onChange={(e) => setIsPublic(e.target.checked)} />
            {Messages.publicRoom()}
          </label>
        </Tooltip>
        <Tooltip title={Messages.hostTooltip()}>
          <span>
            <Button variant="contained" size="small" disabled={disabled || hashing} onClick={() => onHost(isPublic)}>
              {starting ? Messages.opening() : Messages.host()}
            </Button>
          </span>
        </Tooltip>
        <span className={styles.separator} />
        <TextField
          size="small"
          variant="outlined"
          fullWidth={false}
          sx={{ width: 140 }}
          placeholder={Messages.roomCode()}
          value={code}
          inputProps={{ maxLength: 16, "aria-label": Messages.roomCode() }}
          onChange={(e) => setCode(e.target.value.toUpperCase())}
          onKeyDown={(e) => {
            if (e.key === "Enter" && code.trim() && !disabled && !hashing) {
              onJoin(code.trim());
            }
          }}
          className={styles.codeInput}
        />
        <Tooltip title={Messages.joinTooltip()}>
          <span>
            <Button
              variant="outlined"
              size="small"
              disabled={disabled || hashing || !code.trim()}
              onClick={() => onJoin(code.trim())}
            >
              {Messages.join()}
            </Button>
          </span>
        </Tooltip>
      </div>
    </div>
  );
};

const RoomPanel = ({
  status,
  onRequest,
  onCopyCode,
}: {
  status: ModRoomStatus | null;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
}) => {
  const room = status?.room ?? null;
  const isHost = !!room && room.host === room.you;
  const sessionError = status?.session?.phase === "error" ? status.session.error : undefined;
  let title: string = Messages.yourRoom();
  if (room) {
    title = Messages.inRoom(room.code);
  } else if (status?.kind === "join") {
    title = Messages.joiningRoom();
  } else if (!status) {
    title = Messages.opening();
  }
  return (
    <ContentBlock
      title={title}
      titleAction={
        room ? (
          <div className={styles.roomActions}>
            <Tooltip title={Messages.copyCode()}>
              <IconButton size="small" aria-label={Messages.copyCode()} onClick={() => onCopyCode(room.code)}>
                <ContentCopyRoundedIcon fontSize="small" />
              </IconButton>
            </Tooltip>
            {isHost && (
              <Tooltip title={Messages.publicRoomTooltip()}>
                <label className={styles.toggle}>
                  <Switch
                    size="small"
                    checked={room.public}
                    onChange={(e) => onRequest({ op: "public", public: e.target.checked })}
                  />
                  {Messages.publicRoom()}
                </label>
              </Tooltip>
            )}
            <Tooltip title={Messages.leaveTooltip()}>
              <Button size="small" color="secondary" onClick={() => onRequest({ op: "leave" })}>
                {Messages.leave()}
              </Button>
            </Tooltip>
          </div>
        ) : undefined
      }
      content={
        <div className={styles.room}>
          {status?.error && <div className={styles.error}>{status.error}</div>}
          {sessionError && <div className={styles.error}>{Messages.sessionError(sessionError)}</div>}
          {status?.state === "left" && <p className={styles.empty}>{Messages.left()}</p>}
          {!room && !status?.error && <LinearProgress />}
          {room && (
            <>
              <div className={styles.roomBuild}>{Messages.buildName(room.build.name, room.build.hash.slice(0, 8))}</div>
              <div className={styles.slots}>
                {room.slots.map((slot) => (
                  <SlotRow
                    key={slot.slot}
                    slot={slot}
                    isHost={isHost}
                    isYou={slot.slot === room.you}
                    isRoomHost={slot.slot === room.host}
                    rttMs={status?.session?.peers?.find((p) => p.slot === slot.slot)?.rttMs}
                    onRequest={onRequest}
                  />
                ))}
              </div>
            </>
          )}
        </div>
      }
    />
  );
};

const SlotRow = ({
  slot,
  isHost,
  isYou,
  isRoomHost,
  rttMs,
  onRequest,
}: {
  slot: ModRoomSlot;
  isHost: boolean;
  isYou: boolean;
  isRoomHost: boolean;
  rttMs: number | undefined;
  onRequest: (request: ModRoomRequest) => void;
}) => {
  const p = slot.player;
  return (
    <div className={clsx(styles.slot, !slot.open && styles.closed)}>
      <span className={styles.slotLabel}>{Messages.slot(slot.slot)}</span>
      {p ? (
        <span className={styles.player}>
          <span className={styles.name}>{p.displayName}</span>
          <span className={styles.code}>{p.connectCode}</span>
          <span className={styles.badge}>
            {p.controller === "wiimote" ? Messages.wiiRemote() : Messages.gameCube()}
          </span>
          {isRoomHost && <span className={styles.badge}>{Messages.hostLabel()}</span>}
          {isYou && <span className={styles.badge}>{Messages.you()}</span>}
          {!p.connected && <span className={styles.badge}>{Messages.joiningPlayer()}</span>}
          {rttMs !== undefined && rttMs >= 0 && !isYou && <span className={styles.ping}>{Messages.ping(rttMs)}</span>}
        </span>
      ) : (
        <span className={styles.emptySlot}>{slot.open ? Messages.openSlot() : Messages.closedSlot()}</span>
      )}
      {isHost && !isYou && (
        <span className={styles.slotActions}>
          {p ? (
            <Tooltip title={Messages.kickTooltip()}>
              <Button size="small" color="secondary" onClick={() => onRequest({ op: "kick", slot: slot.slot })}>
                {Messages.kick()}
              </Button>
            </Tooltip>
          ) : (
            <Button size="small" onClick={() => onRequest({ op: "slot", slot: slot.slot, open: !slot.open })}>
              {slot.open ? Messages.closeSlot() : Messages.openSlotAction()}
            </Button>
          )}
        </span>
      )}
    </div>
  );
};
