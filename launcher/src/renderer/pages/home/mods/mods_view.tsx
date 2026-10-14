import type { PublicModRoom } from "@accounts/types";
import type {
  ModBuild,
  ModRoomRequest,
  ModRoomSlot,
  ModRoomStatus,
  ModsController,
  ModsState,
} from "@dolphin/mods/types";
import { defaultController } from "@dolphin/mods/types";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import ContentCopyRoundedIcon from "@mui/icons-material/ContentCopyRounded";
import CreateNewFolderOutlinedIcon from "@mui/icons-material/CreateNewFolderOutlined";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import DragIndicatorRoundedIcon from "@mui/icons-material/DragIndicatorRounded";
import PersonOutlineIcon from "@mui/icons-material/PersonOutline";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import type { ReactNode } from "react";
import { useEffect, useState } from "react";

import { ExternalLink } from "@/components/external_link";

import { ContentBlock } from "../overview/content_block/content_block";
import { ControllerMenu } from "./controller_menu";
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
  /** The builds being hashed, by id: the share done (0-1), or null while it is not known. */
  hashProgress: Record<string, number | null>;
  /** A build whose host or join is starting Dolphin. */
  startingBuildId: string | null;
  error: string | null;
  onControllerChange: (controller: ModsController) => void;
  onConfigureControllers: () => void;
  onRefreshControllers: () => void;
  onAddBuild: () => void;
  onAddFolder: () => void;
  onRemoveBuild: (build: ModBuild) => void;
  onHost: (build: ModBuild, isPublic: boolean) => void;
  onJoin: (build: ModBuild, code: string) => void;
  /** A room code typed by the public rooms (its game is looked up). */
  onJoinCode: (code: string) => void;
  /** A public room whose game the player does not have. */
  onMissingGame: (name: string, hash: string) => void;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
};

const fileName = (p: string | null) => (p ? p.split(/[\\/]/).pop() ?? p : null);

/** Where Wii Rollback's beta feedback goes. */
const FEEDBACK_URL = "https://brawlonline.net/discord";
/** How long the public switch shows a click the room file does not say yet. */
const PUBLIC_WAIT_MS = 5000;

/** Home > Other Games (docs/design/brawl-mods.md §8): the player's games and mods, and the room Dolphin runs. */
export const ModsView = (props: ModsViewProps) => {
  const { state, error } = props;
  const status = state?.running ? state.status : null;
  // The running room's game, if it is one of ours: what it is made for.
  const roomBuild = status?.room ? state?.builds.find((b) => b.hash === status.room!.build.hash) : undefined;
  // Why the last Dolphin closed (a kick, a failed join), until the player closes the message; a
  // new room clears it anyway.
  const [dismissed, setDismissed] = useState<string | null>(null);
  const running = !!state?.running;
  useEffect(() => {
    if (running) {
      setDismissed(null);
    }
  }, [running]);
  const shownError = error ?? (!state?.running && state?.lastError !== dismissed ? state?.lastError : null);
  return (
    <div className={styles.page}>
      <div className={styles.introRow}>
        <p className={styles.intro}>{Messages.intro()}</p>
        <p className={styles.beta}>
          {Messages.betaLine1()}
          <br />
          {Messages.betaLine2Before()}
          <ExternalLink href={FEEDBACK_URL}>{Messages.discord()}</ExternalLink>
        </p>
      </div>
      {state && (
        <ControllerMenu
          controller={state.controller}
          controllers={state.controllers}
          input={status?.input}
          gameWants={roomBuild ? defaultController(roomBuild) : null}
          onChange={props.onControllerChange}
          onConfigure={props.onConfigureControllers}
          onOpen={props.onRefreshControllers}
        />
      )}
      {shownError && (
        <div className={clsx(styles.error, styles.dismissable)}>
          <span>{shownError}</span>
          {!error && (
            <IconButton
              size="small"
              aria-label={Messages.dismiss()}
              onClick={() => setDismissed(state?.lastError ?? null)}
            >
              <CloseRoundedIcon fontSize="small" />
            </IconButton>
          )}
        </div>
      )}
      <div className={styles.columns}>
        {/* Left: the public rooms, under the player's own room while Dolphin runs one. */}
        <div className={styles.column}>
          {state?.running && (
            <div className={styles.roomSlot}>
              <RoomPanel status={status} onRequest={props.onRequest} onCopyCode={props.onCopyCode} />
            </div>
          )}
          <PublicRoomsBlock
            rooms={props.publicRooms}
            ownCode={status?.room?.code ?? null}
            builds={state?.builds ?? []}
            disabled={!!state?.running || props.startingBuildId !== null}
            onJoin={props.onJoin}
            onJoinCode={props.onJoinCode}
            onMissingGame={props.onMissingGame}
            onLogIn={props.onLogIn}
          />
        </div>
        {/* Right: the player's games. */}
        <div className={styles.column}>
          <ContentBlock
            fill={true}
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
                    onHost={(isPublic) => props.onHost(build, isPublic)}
                  />
                ))}
              </div>
            }
          />
        </div>
      </div>
    </div>
  );
};

const PublicRoomsBlock = ({
  rooms,
  ownCode,
  builds,
  disabled,
  onJoin,
  onJoinCode,
  onMissingGame,
  onLogIn,
}: {
  rooms: ModsPublicRooms;
  /** The room this player is in: listed, marked as theirs. */
  ownCode: string | null;
  builds: ModBuild[];
  disabled: boolean;
  onJoin: (build: ModBuild, code: string) => void;
  onJoinCode: (code: string) => void;
  onMissingGame: (name: string, hash: string) => void;
  onLogIn: () => void;
}) => {
  const [code, setCode] = useState("");
  let content: ReactNode;
  const listed = rooms.kind === "ready" ? rooms.rooms : [];
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
  } else if (listed.length === 0) {
    content = <p className={styles.empty}>{Messages.publicRoomsNone()}</p>;
  } else {
    // As Home > Overview's rooms: the whole row joins.
    content = (
      <ul className={styles.roomList}>
        {listed.map((room) => {
          const build = builds.find((b) => b.hash === room.build.hash);
          const label = Messages.buildName(room.build.name, room.build.hash.slice(0, 8));
          const own = room.code === ownCode;
          const off = disabled || own;
          return (
            <li key={room.code}>
              <Tooltip
                title={
                  own
                    ? Messages.yourRoomTooltip()
                    : build
                    ? Messages.joinPublicTooltip(build.name)
                    : Messages.needBuild(room.build.name, room.build.hash.slice(0, 8))
                }
                placement="top"
                enterDelay={600}
                disableInteractive={true}
              >
                <button
                  type="button"
                  className={clsx(styles.roomRow, !build && styles.roomRowOff)}
                  aria-disabled={off}
                  data-room={room.code}
                  onClick={() => {
                    if (off) {
                      return;
                    }
                    if (build) {
                      onJoin(build, room.code);
                    } else {
                      onMissingGame(room.build.name, room.build.hash);
                    }
                  }}
                >
                  <div className={styles.roomMain}>
                    <div className={styles.roomHost}>{Messages.roomHost(room.hostName)}</div>
                    <div className={styles.roomGame}>{label}</div>
                  </div>
                  <span className={styles.roomPlayers}>
                    <PersonOutlineIcon style={{ fontSize: 16 }} />
                    {Messages.roomPlayers(room.players, room.open)}
                  </span>
                  <span className={styles.roomAction}>
                    {own ? (
                      Messages.yourRoomLabel()
                    ) : build ? (
                      <>
                        {Messages.join()}
                        <ChevronRightIcon style={{ fontSize: 18 }} />
                      </>
                    ) : (
                      Messages.noGame()
                    )}
                  </span>
                </button>
              </Tooltip>
            </li>
          );
        })}
      </ul>
    );
  }
  const join = () => {
    if (code.trim() && !disabled) {
      onJoinCode(code.trim());
    }
  };
  return (
    <ContentBlock
      fill={true}
      title={Messages.publicRooms()}
      titleAction={
        rooms.kind !== "logged-out" ? (
          <div className={styles.roomActions}>
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
                if (e.key === "Enter") {
                  join();
                }
              }}
            />
            <Tooltip title={Messages.joinTooltip()}>
              <span>
                <Button variant="outlined" size="small" disabled={disabled || !code.trim()} onClick={join}>
                  {Messages.join()}
                </Button>
              </span>
            </Tooltip>
          </div>
        ) : undefined
      }
      content={<div className={styles.room}>{content}</div>}
    />
  );
};

const BuildRow = ({
  build,
  progress,
  starting,
  disabled,
  onRemove,
  onHost,
}: {
  build: ModBuild;
  /** Hashing: the share done, null while not known; undefined when not hashing. */
  progress: number | null | undefined;
  starting: boolean;
  disabled: boolean;
  onRemove: () => void;
  onHost: (isPublic: boolean) => void;
}) => {
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
          {progress === null ? (
            <LinearProgress variant="indeterminate" />
          ) : (
            <LinearProgress variant="determinate" value={progress * 100} />
          )}
          <span>{progress === null ? Messages.checkingDisc() : Messages.checking(Math.floor(progress * 100))}</span>
        </div>
      )}
      <div className={styles.actions}>
        <Tooltip title={Messages.publicRoomTooltip()}>
          <label className={styles.toggle}>
            <Switch size="small" checked={isPublic} onChange={(e) => setIsPublic(e.target.checked)} />
            {Messages.publicRoom()}
          </label>
        </Tooltip>
        <Tooltip title={Messages.hostTooltip()}>
          <span>
            <Button variant="contained" size="small" disabled={disabled || hashing} onClick={() => onHost(isPublic)}>
              {starting && !hashing ? Messages.opening() : Messages.host()}
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
  const [dragFrom, setDragFrom] = useState<number | null>(null);
  // The public switch moves at the click: the room file says it only after Dolphin and the
  // server have (up to ~1.5 s of file polls plus the round trip). Back to the room's value once
  // it agrees, or when it never does (refused, lost).
  const [publicClicked, setPublicClicked] = useState<boolean | null>(null);
  const room = status?.room ?? null;
  const isHost = !!room && room.host === room.you;
  const roomPublic = room?.public;
  useEffect(() => {
    if (publicClicked === null) {
      return;
    }
    if (roomPublic === publicClicked) {
      setPublicClicked(null);
      return;
    }
    const timer = setTimeout(() => setPublicClicked(null), PUBLIC_WAIT_MS);
    return () => clearTimeout(timer);
  }, [publicClicked, roomPublic]);
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
                    checked={publicClicked ?? room.public}
                    onChange={(e) => {
                      setPublicClicked(e.target.checked);
                      onRequest({ op: "public", public: e.target.checked });
                    }}
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
          {status?.lastRequest?.result === "failed" && status.lastRequest.error && (
            <div className={styles.error}>{status.lastRequest.error}</div>
          )}
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
                    dragging={dragFrom}
                    onDragFrom={setDragFrom}
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
  dragging,
  onDragFrom,
  onRequest,
}: {
  slot: ModRoomSlot;
  isHost: boolean;
  isYou: boolean;
  isRoomHost: boolean;
  rttMs: number | undefined;
  /** The slot whose player the host is dragging, if any. */
  dragging: number | null;
  onDragFrom: (slot: number | null) => void;
  onRequest: (request: ModRoomRequest) => void;
}) => {
  const p = slot.player;
  // The host drags a player to any other slot: an empty one (open or closed), or another
  // player's, and the two swap.
  const draggable = isHost && !!p;
  const target = isHost && dragging !== null && dragging !== slot.slot;
  const [over, setOver] = useState(false);
  return (
    <div
      className={clsx(
        styles.slot,
        !slot.open && styles.closed,
        draggable && styles.draggable,
        target && over && styles.dropTarget,
      )}
      draggable={draggable}
      data-slot={slot.slot}
      onDragStart={(e) => {
        e.dataTransfer.setData("text/plain", String(slot.slot));
        e.dataTransfer.effectAllowed = "move";
        window.setTimeout(() => onDragFrom(slot.slot), 0);
      }}
      onDragEnd={() => onDragFrom(null)}
      onDragOver={(e) => {
        if (target || (isHost && dragging === null)) {
          e.preventDefault();
          e.dataTransfer.dropEffect = "move";
          setOver(true);
        }
      }}
      onDragLeave={() => setOver(false)}
      onDrop={(e) => {
        e.preventDefault();
        setOver(false);
        // The dragged slot, from the drag's data (the state may not have caught up).
        const from = dragging ?? Number(e.dataTransfer.getData("text/plain"));
        if (isHost && from >= 1 && from <= 4 && from !== slot.slot) {
          onRequest({ op: "move", from, to: slot.slot });
        }
        onDragFrom(null);
      }}
    >
      {draggable ? (
        <Tooltip title={Messages.dragTooltip()} placement="left" enterDelay={600} disableInteractive={true}>
          <DragIndicatorRoundedIcon className={styles.dragHandle} fontSize="small" />
        </Tooltip>
      ) : (
        isHost && <span className={styles.dragSpacer} />
      )}
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
              <Button
                size="small"
                color="secondary"
                sx={{ py: 0 }}
                onClick={() => onRequest({ op: "kick", slot: slot.slot })}
              >
                {Messages.kick()}
              </Button>
            </Tooltip>
          ) : (
            <Button
              size="small"
              sx={{ py: 0 }}
              onClick={() => onRequest({ op: "slot", slot: slot.slot, open: !slot.open })}
            >
              {slot.open ? Messages.closeSlot() : Messages.openSlotAction()}
            </Button>
          )}
        </span>
      )}
    </div>
  );
};
