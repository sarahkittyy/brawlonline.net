import type { PublicModRoom } from "@accounts/types";
import type { PartMatch } from "@dolphin/mods/room_match";
import { matchRoom } from "@dolphin/mods/room_match";
import type {
  ModBuild,
  ModRoomRequest,
  ModRoomSlot,
  ModRoomStatus,
  ModsController,
  ModSdSync,
  ModsState,
} from "@dolphin/mods/types";
import { defaultController } from "@dolphin/mods/types";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import CheckRoundedIcon from "@mui/icons-material/CheckRounded";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import CloseRoundedIcon from "@mui/icons-material/CloseRounded";
import ContentCopyRoundedIcon from "@mui/icons-material/ContentCopyRounded";
import CreateNewFolderOutlinedIcon from "@mui/icons-material/CreateNewFolderOutlined";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import DragIndicatorRoundedIcon from "@mui/icons-material/DragIndicatorRounded";
import EditOutlinedIcon from "@mui/icons-material/EditOutlined";
import PersonOutlineIcon from "@mui/icons-material/PersonOutline";
import Button from "@mui/material/Button";
import Dialog from "@mui/material/Dialog";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
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
  onEditBuild: (build: ModBuild) => void;
  onRemoveBuild: (build: ModBuild) => void;
  onHost: (build: ModBuild, isPublic: boolean) => void;
  onJoin: (build: ModBuild, code: string) => void;
  /** A room code typed by the public rooms (its game is looked up). */
  onJoinCode: (code: string) => void;
  /** A public room whose game the player does not have. */
  onMissingGame: (name: string, hash: string) => void;
  /** A room's part that differs from the player's game: its edit window, that field pulsing. */
  onFixBuild: (build: ModBuild, part: "disc" | "sd") => void;
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
  const roomBuild = status?.room ? matchRoom(status.room.build, state?.builds ?? []).build ?? undefined : undefined;
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
            onFixBuild={props.onFixBuild}
            onLogIn={props.onLogIn}
          />
        </div>
        {/* Right: the player's games. */}
        <div className={styles.column}>
          <ContentBlock
            fill={true}
            title={Messages.yourBuilds()}
            titleAction={
              <>
                <div className={styles.addButtons}>
                  <Tooltip title={Messages.addBuildTooltip()}>
                    <Button
                      size="small"
                      classes={{ startIcon: styles.addIcon }}
                      startIcon={<AddRoundedIcon />}
                      onClick={props.onAddBuild}
                    >
                      <span className={styles.addLabel}>{Messages.addBuild()}</span>
                    </Button>
                  </Tooltip>
                  <Tooltip title={Messages.addFolderTooltip()}>
                    <Button
                      size="small"
                      classes={{ startIcon: styles.addIcon }}
                      startIcon={<CreateNewFolderOutlinedIcon />}
                      onClick={props.onAddFolder}
                    >
                      <span className={styles.addLabel}>{Messages.addFolder()}</span>
                    </Button>
                  </Tooltip>
                </div>
              </>
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
                    onEdit={() => props.onEditBuild(build)}
                    onRemove={() => props.onRemoveBuild(build)}
                    onHost={(isPublic) => props.onHost(build, isPublic)}
                  />
                ))}
              </div>
            }
          />
        </div>
      </div>
      <SdSyncDialog sync={status?.session?.sd_sync ?? null} onRequest={props.onRequest} />
    </div>
  );
};

/** An SD card difference this big is rarely a few changed files: more likely another release of the
 * mod (the user's, 2026-10-19), so the question and the download say so. Dolphin's chat uses the
 * same 50 MB (MachineSession.cpp SD_BIG_DIFFERENCE_BYTES). */
export const SD_BIG_DIFFERENCE_BYTES = 50 * 1024 * 1024;

/** Dolphin asks about the SD card itself (docs/design/brawl-mods.md §6.5) only when the card turned
 * out different there after the launcher's own check before the join (SdCardDialog in mods_tab):
 * download the host's card, or leave. Dolphin waits for the answer. */
const SdSyncDialog = ({
  sync,
  onRequest,
}: {
  sync: ModSdSync | null;
  onRequest: (request: ModRoomRequest) => void;
}) => {
  const asking = sync?.step === "ask";
  // Answered: closed until Dolphin asks again (its file says "ask" until it read the answer).
  const [answered, setAnswered] = useState(false);
  useEffect(() => {
    if (!asking) {
      setAnswered(false);
    }
  }, [asking]);
  if (!sync) {
    return null;
  }
  return (
    <SdCardDialog
      open={asking && !answered}
      body={Messages.sdSyncBody(Messages.megabytes(sync.bytes), sync.files, Messages.duration(sync.eta_s))}
      bigDifference={sync.bytes > SD_BIG_DIFFERENCE_BYTES}
      names={sync.names}
      files={sync.files}
      onAnswer={(choice) => {
        setAnswered(true);
        onRequest({ op: "sd-sync", choice });
      }}
    />
  );
};

/** The SD card question (a room's card that differs from the player's): how much differs, the
 * warning, the largest files that differ, and No / Yes / Yes, and overwrite local card. */
export const SdCardDialog = ({
  open,
  body,
  bigDifference,
  names,
  files,
  onAnswer,
}: {
  open: boolean;
  body: string;
  /** Over SD_BIG_DIFFERENCE_BYTES: "If it's too big, ..." after the body. */
  bigDifference: boolean;
  /** Some of the files that differ, largest first. */
  names: string[];
  /** How many files differ in all. */
  files: number;
  onAnswer: (choice: "yes" | "overwrite" | "no") => void;
}) => {
  const shown = names.slice(0, 8);
  const more = Math.max(files, names.length) - shown.length;
  return (
    <Dialog open={open} maxWidth="sm" fullWidth={true}>
      <DialogTitle>{Messages.sdSyncTitle()}</DialogTitle>
      <DialogContent>
        <p className={styles.sdSyncBody}>
          {body}
          {bigDifference && ` ${Messages.sdBigDifferenceQuestion()}`}
        </p>
        <p className={styles.sdSyncWarning}>{Messages.sdSyncWarning()}</p>
        {shown.length > 0 && (
          <div className={styles.sdSyncFiles}>
            <div>{Messages.sdSyncFiles()}</div>
            <ul>
              {shown.map((name) => (
                <li key={name}>{name}</li>
              ))}
            </ul>
            {more > 0 && <div>{Messages.sdSyncMoreFiles(more)}</div>}
          </div>
        )}
      </DialogContent>
      <DialogActions>
        <Button onClick={() => onAnswer("no")}>{Messages.sdSyncNo()}</Button>
        <Button variant="outlined" onClick={() => onAnswer("yes")}>
          {Messages.sdSyncYes()}
        </Button>
        <Button variant="contained" onClick={() => onAnswer("overwrite")}>
          {Messages.sdSyncOverwrite()}
        </Button>
      </DialogActions>
    </Dialog>
  );
};

/** One part of a game as rooms compare it ("Brawl: 3f9a1c"), with how the player's compares. */
const PartChip = ({
  label,
  hash,
  match,
  tooltip,
  onClick,
}: {
  label: string;
  hash: string;
  /** null: shown as it is (the player's own game). */
  match: PartMatch | null;
  tooltip: string;
  onClick?: () => void;
}) => (
  <Tooltip title={tooltip} disableInteractive={true}>
    <span
      role={onClick ? "button" : undefined}
      tabIndex={onClick ? 0 : undefined}
      className={clsx(
        styles.partChip,
        match === "same" && styles.partSame,
        match === "differs" && styles.partDiffers,
        match === "wrong" && styles.partWrong,
        onClick && styles.partClickable,
      )}
      onClick={(e) => {
        if (onClick) {
          e.stopPropagation();
          onClick();
        }
      }}
      onKeyDown={(e) => {
        if (onClick && (e.key === "Enter" || e.key === " ")) {
          e.preventDefault();
          e.stopPropagation();
          onClick();
        }
      }}
    >
      {Messages.partHash(label, hash)}
      {match === "same" && <CheckRoundedIcon className={styles.partIcon} />}
      {match === "wrong" && <CloseRoundedIcon className={styles.partIcon} />}
      {match === "differs" && <span className={styles.partIcon}>!</span>}
    </span>
  </Tooltip>
);

const PublicRoomsBlock = ({
  rooms,
  ownCode,
  builds,
  disabled,
  onJoin,
  onJoinCode,
  onMissingGame,
  onFixBuild,
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
  onFixBuild: (build: ModBuild, part: "disc" | "sd") => void;
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
          const match = matchRoom(room.build, builds);
          const build = match.joinable ? match.build : null;
          const label = match.hasParts
            ? room.build.name
            : Messages.buildName(room.build.name, room.build.hash.slice(0, 8));
          const own = room.code === ownCode;
          const off = disabled || own;
          const gameId = room.build.discLabel?.split(" ")[0];
          const fix = (part: "disc" | "sd") => () => {
            if (match.build) {
              onFixBuild(match.build, part);
            } else {
              onMissingGame(room.build.name, room.build.hash);
            }
          };
          return (
            <li key={room.code}>
              <Tooltip
                title={
                  own
                    ? Messages.yourRoomTooltip()
                    : build
                    ? Messages.joinPublicTooltip(build.name)
                    : match.hasParts && match.disc === "wrong"
                    ? Messages.discDiffers(room.build.discLabel)
                    : match.hasParts && match.sd === "wrong"
                    ? Messages.sdMissing()
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
                    } else if (match.build && match.hasParts) {
                      fix(match.disc === "wrong" ? "disc" : "sd")();
                    } else {
                      onMissingGame(room.build.name, room.build.hash);
                    }
                  }}
                >
                  <div className={styles.roomMain}>
                    <div className={styles.roomHost}>{Messages.roomHost(room.hostName)}</div>
                    <div className={styles.roomGame}>{label}</div>
                    {match.hasParts && room.build.disc && (
                      <div className={styles.partChips}>
                        <PartChip
                          label={Messages.discPart(gameId)}
                          hash={room.build.disc}
                          match={match.disc}
                          tooltip={
                            match.disc === "same"
                              ? Messages.discMatches()
                              : `${
                                  gameId?.startsWith("RSB")
                                    ? Messages.discDiffers(room.build.discLabel)
                                    : Messages.discDiffersGame(room.build.discLabel)
                                } ${Messages.fixPartTooltip()}`
                          }
                          onClick={match.disc === "same" ? undefined : fix("disc")}
                        />
                        {(room.build.sd || match.sd === "wrong") && (
                          <PartChip
                            label={Messages.sdPart()}
                            hash={room.build.sd || "-"}
                            match={match.sd}
                            tooltip={
                              match.sd === "same"
                                ? Messages.sdMatches()
                                : match.sd === "differs"
                                ? `${Messages.sdDiffers()} ${Messages.fixPartTooltip()}`
                                : `${
                                    room.build.sd ? Messages.sdMissing() : Messages.sdExtra()
                                  } ${Messages.fixPartTooltip()}`
                            }
                            onClick={match.sd === "same" || !match.build ? undefined : fix("sd")}
                          />
                        )}
                      </div>
                    )}
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
                    ) : match.hasParts && match.build && match.disc === "wrong" ? (
                      Messages.noJoinDisc()
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
  onEdit,
  onRemove,
  onHost,
}: {
  build: ModBuild;
  /** Hashing: the share done, null while not known; undefined when not hashing. */
  progress: number | null | undefined;
  starting: boolean;
  disabled: boolean;
  onEdit: () => void;
  onRemove: () => void;
  onHost: (isPublic: boolean) => void;
}) => {
  const [isPublic, setIsPublic] = useState(true);
  const hashing = progress !== undefined;
  return (
    <div className={styles.build}>
      <div className={styles.buildHead}>
        <div className={styles.buildName}>
          <Tooltip title={build.name} enterDelay={600}>
            <span className={styles.name}>{build.name}</span>
          </Tooltip>
          {build.hash && build.parts ? (
            <span className={styles.partChips}>
              <PartChip
                label={Messages.discPart(build.gameId ?? undefined)}
                hash={build.parts.disc}
                match={null}
                tooltip={Messages.partsTooltip()}
              />
              {build.sdPath && build.parts.sdCard && (
                <PartChip
                  label={Messages.sdPart()}
                  hash={build.parts.sdCard}
                  match={null}
                  tooltip={Messages.partsTooltip()}
                />
              )}
            </span>
          ) : build.hash ? (
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
        <Tooltip title={Messages.editTooltip()}>
          <span>
            <IconButton size="small" aria-label={Messages.edit()} onClick={onEdit} disabled={disabled || hashing}>
              <EditOutlinedIcon fontSize="small" />
            </IconButton>
          </span>
        </Tooltip>
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
  const sync = status?.session?.sd_sync;
  let syncLine: string | null = null;
  if (sync?.step === "checking" && sync.checked < 1) {
    syncLine = Messages.sdSyncChecking(Math.floor(sync.checked * 100));
  } else if (sync?.step === "downloading" && sync.total) {
    syncLine = Messages.sdSyncDownloading(
      Math.floor(((sync.done ?? 0) * 100) / sync.total),
      Messages.rate(sync.rate ?? 0),
      Messages.duration(sync.eta_s),
    );
  }
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
          {syncLine && (
            <div className={styles.progress}>
              <LinearProgress
                variant="determinate"
                value={
                  sync?.step === "downloading"
                    ? ((sync.done ?? 0) * 100) / (sync.total || 1)
                    : (sync?.checked ?? 0) * 100
                }
              />
              <span>{syncLine}</span>
            </div>
          )}
          {sync?.step === "downloading" && (sync.total ?? 0) > SD_BIG_DIFFERENCE_BYTES && (
            <div className={styles.sdBigDifference}>{Messages.sdBigDifference()}</div>
          )}
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
