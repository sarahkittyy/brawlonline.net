import type { PublicModRoom, YourRoom } from "@accounts/types";
import type { PartMatch, RoomMatch } from "@dolphin/mods/room_match";
import { matchRoom } from "@dolphin/mods/room_match";
import type {
  ModBuild,
  ModRoomRequest,
  ModRoomSlot,
  ModRoomStatus,
  ModsController,
  ModSdCardCheck,
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
import { useEffect, useRef, useState } from "react";

import { ExternalLink } from "@/components/external_link";
import { RoomNameMessages } from "@/components/room_name/room_name.messages";
import { RoomNameField } from "@/components/room_name/room_name_field";
import { UserName } from "@/components/user_name/user_name";

import { ContentBlock } from "../overview/content_block/content_block";
import { CodesButton, CodesChip, hasRoomCodes } from "./codes_dialog";
import { ControllerMenu } from "./controller_menu";
import type { CardCheckState } from "./join_with_menu";
import { cardCheckKey, JoinsWith, JoinWithMenu } from "./join_with_menu";
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
  /** The game's Codes dialog (docs/design/room-codes.md). */
  onCodes?: (build: ModBuild) => void;
  onHost: (build: ModBuild, isPublic: boolean) => void;
  /** `discOnly`: the room plays `build`'s disc alone (room_match.ts). */
  onJoin: (build: ModBuild, code: string, discOnly?: boolean) => void;
  /** A room code typed by the public rooms (its game is looked up). */
  onJoinCode: (code: string) => void;
  /** A public room whose game the player does not have. */
  onMissingGame: (name: string, hash: string) => void;
  /** A room's part that differs from the player's game: its edit window, that field pulsing. */
  onFixBuild: (build: ModBuild, part: "disc" | "sd") => void;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
  /** The room the player is in, as the server lists it (its name; whether they host it). */
  yourRoom?: YourRoom | null;
  /** The host names their room (empty: no name); throws when the server refused it. */
  onRenameRoom?: (code: string, name: string) => Promise<void>;
  /** An admin account: every room has the pencil, and names it as its host would. */
  onAdminRenameRoom?: (code: string, name: string) => Promise<void>;
  /** "Joins with": rooms of the mod whose build hash is `roomHash` join with this game from now on. */
  onSetRoomGame?: (roomHash: string, buildId: string) => void;
  /** What joining a room whose card is `roomRoot` downloads with each game, by build id (logged in). */
  checkRoomCards?: (roomRoot: string, buildIds: string[]) => Promise<Record<string, ModSdCardCheck>>;
};

const fileName = (p: string | null) => (p ? p.split(/[\\/]/).pop() ?? p : null);

/** Where Wii/GCN Rollback's beta feedback goes. */
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
              <RoomPanel
                status={status}
                onRequest={props.onRequest}
                onCopyCode={props.onCopyCode}
                yourRoom={props.yourRoom}
                onRename={props.onRenameRoom}
                onAdminRename={props.onAdminRenameRoom}
                onCodes={roomBuild && props.onCodes ? () => props.onCodes?.(roomBuild) : undefined}
              />
            </div>
          )}
          <PublicRoomsBlock
            rooms={props.publicRooms}
            ownCode={status?.room?.code ?? null}
            builds={state?.builds ?? []}
            discAlone={state?.discAlone}
            roomGames={state?.roomGames}
            onSetRoomGame={props.onSetRoomGame}
            checkRoomCards={props.checkRoomCards}
            disabled={!!state?.running || props.startingBuildId !== null}
            onJoin={props.onJoin}
            onJoinCode={props.onJoinCode}
            onMissingGame={props.onMissingGame}
            onFixBuild={props.onFixBuild}
            onLogIn={props.onLogIn}
            onAdminRename={props.onAdminRenameRoom}
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
                    onCodes={props.onCodes ? () => props.onCodes?.(build) : undefined}
                    onHost={(isPublic) => props.onHost(build, isPublic)}
                  />
                ))}
              </div>
            }
          />
        </div>
      </div>
      <SdSyncDialog
        sync={status?.session?.sd_sync ?? null}
        build={state?.builds.find((b) => b.id === state.runningBuildId) ?? null}
        roomName={status?.room?.build.name}
        onRequest={props.onRequest}
      />
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
  build,
  roomName,
  onRequest,
}: {
  sync: ModSdSync | null;
  /** The player's game the room plays, if known. */
  build: ModBuild | null;
  roomName?: string;
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
      build={build}
      roomName={roomName}
      onAnswer={(choice) => {
        setAnswered(true);
        onRequest({ op: "sd-sync", choice });
      }}
    />
  );
};

/** The SD card question (a room's card that differs from the player's): which of the player's
 * games joins and the room's, how much differs, the warning, the largest files that differ, and
 * No / Yes / Yes, and overwrite <the game>'s card. Brawl Online's own P+ has no overwrite (its card
 * stays P+'s; Sarah's board, 2026-10-29). */
export const SdCardDialog = ({
  open,
  body,
  bigDifference,
  names,
  files,
  build,
  roomName,
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
  /** The player's game whose card it is, when known. */
  build?: ModBuild | null;
  /** The room's game, as its host named it. */
  roomName?: string;
  onAnswer: (choice: "yes" | "overwrite" | "no") => void;
}) => {
  const shown = names.slice(0, 8);
  const more = Math.max(files, names.length) - shown.length;
  return (
    <Dialog open={open} maxWidth="sm" fullWidth={true}>
      <DialogTitle>{Messages.sdSyncTitle()}</DialogTitle>
      <DialogContent>
        {build && <p className={styles.sdSyncBody}>{Messages.sdSyncGames(build.name, roomName)}</p>}
        <p className={styles.sdSyncBody}>
          {body}
          {bigDifference && ` ${Messages.sdBigDifferenceQuestion()}`}
        </p>
        {build?.builtIn && <p className={styles.sdSyncBody}>{Messages.sdSyncBuiltInKept(build.name)}</p>}
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
        <Button variant={build?.builtIn ? "contained" : "outlined"} onClick={() => onAnswer("yes")}>
          {Messages.sdSyncYes()}
        </Button>
        {!build?.builtIn && (
          <Button variant="contained" onClick={() => onAnswer("overwrite")}>
            {build ? Messages.sdSyncOverwriteGame(build.name) : Messages.sdSyncOverwrite()}
          </Button>
        )}
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
  extra,
}: {
  label: string;
  hash: string;
  /** After the hash: what joining downloads of it ("15 MB"). */
  extra?: string;
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
      {extra && ` · ${extra}`}
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
  discAlone,
  roomGames,
  onSetRoomGame,
  checkRoomCards,
  disabled,
  onJoin,
  onJoinCode,
  onMissingGame,
  onFixBuild,
  onLogIn,
  onAdminRename,
}: {
  rooms: ModsPublicRooms;
  /** The room this player is in: listed, marked as theirs. */
  ownCode: string | null;
  builds: ModBuild[];
  /** The build hash of each of the player's discs played alone (ModsState.discAlone). */
  discAlone?: Record<string, string>;
  /** The game the player picked for each room mod (ModsState.roomGames). */
  roomGames?: Record<string, string>;
  onSetRoomGame?: (roomHash: string, buildId: string) => void;
  checkRoomCards?: (roomRoot: string, buildIds: string[]) => Promise<Record<string, ModSdCardCheck>>;
  disabled: boolean;
  onJoin: (build: ModBuild, code: string, discOnly?: boolean) => void;
  onJoinCode: (code: string) => void;
  onMissingGame: (name: string, hash: string) => void;
  onFixBuild: (build: ModBuild, part: "disc" | "sd") => void;
  onLogIn: () => void;
  /** An admin account: each room's pencil, as its host has. */
  onAdminRename?: (code: string, name: string) => Promise<void>;
}) => {
  const [code, setCode] = useState("");
  // "Joins with": a game picked for one room only (by its code), each game's card against a
  // room's, and the open menu (`joinAfter`: opened by Join, which joins once a game is picked).
  const [pickedHere, setPickedHere] = useState<Record<string, string>>({});
  const [checks, setChecks] = useState<Record<string, CardCheckState>>({});
  const [menu, setMenu] = useState<{ code: string; anchor: HTMLElement; joinAfter: boolean } | null>(null);
  const anchors = useRef(new Map<string, HTMLElement>());
  let content: ReactNode;
  const listed = rooms.kind === "ready" ? rooms.rooms : [];
  const matchOf = (room: PublicModRoom) =>
    matchRoom(room.build, builds, (disc) => discAlone?.[disc], pickedHere[room.code] ?? roomGames?.[room.build.hash]);
  const openMenu = (room: PublicModRoom, match: RoomMatch, anchor: HTMLElement, joinAfter: boolean) => {
    setMenu({ code: room.code, anchor, joinAfter });
    const root = room.build.sd;
    if (!root || !checkRoomCards) {
      return;
    }
    // The sizes: each game whose card is not the room's, asked once (the main process compares
    // them with the room card's listing, as the SD card question at a join does).
    const need = match.choices.filter(
      (b) => b.sdPath && b.parts?.sdCard !== root && checks[cardCheckKey(root, b)] === undefined,
    );
    if (need.length === 0) {
      return;
    }
    const keyed = need.map((b) => [cardCheckKey(root, b), b.id] as const);
    setChecks((c) => ({ ...c, ...Object.fromEntries(keyed.map(([k]) => [k, "checking" as const])) }));
    void checkRoomCards(
      root,
      need.map((b) => b.id),
    )
      .catch(() => ({} as Record<string, ModSdCardCheck>))
      .then((result) =>
        setChecks((c) => ({
          ...c,
          ...Object.fromEntries(keyed.map(([k, id]) => [k, result[id] ?? ({ kind: "unknown" } as const)])),
        })),
      );
  };
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
          const match = matchOf(room);
          const build = match.joinable ? match.build : null;
          // Joining with a game that is only a guess (none is the room's mod or has its card)
          // asks which one first.
          const ask = !!build && !match.sure && match.choices.length > 1;
          const cardCheck = build && room.build.sd ? checks[cardCheckKey(room.build.sd, build)] : undefined;
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
                    : ask
                    ? Messages.joinWithPickTooltip()
                    : build
                    ? Messages.joinPublicTooltip(match.discOnly ? room.build.name : build.name)
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
                  onClick={(e) => {
                    if (off) {
                      return;
                    }
                    if (ask) {
                      openMenu(room, match, anchors.current.get(room.code) ?? e.currentTarget, true);
                    } else if (build) {
                      if (match.discOnly) {
                        onJoin(build, room.code, true);
                      } else {
                        onJoin(build, room.code);
                      }
                    } else if (match.build && match.hasParts) {
                      fix(match.disc === "wrong" ? "disc" : "sd")();
                    } else {
                      onMissingGame(room.build.name, room.build.hash);
                    }
                  }}
                >
                  <div className={styles.roomMain}>
                    {onAdminRename ? (
                      <div className={styles.roomHost}>
                        <RoomNameField
                          name={room.name ?? null}
                          fallback={Messages.roomHost(room.hostName)}
                          canEdit={true}
                          onSave={async (name) => onAdminRename(room.code, name)}
                          inButton={true}
                        />
                      </div>
                    ) : (
                      // A room its host didn't name: "alice's room" in grey, so a real name stands out.
                      <div className={clsx(styles.roomHost, !room.name && styles.roomUnnamed)}>
                        {room.name ?? Messages.roomHost(room.hostName)}
                      </div>
                    )}
                    <div className={styles.roomGame}>
                      <span className={styles.crown} title={Messages.hostLabel()}>
                        {"♛ "}
                      </span>
                      <UserName
                        user={{ uid: room.hostUid, displayName: room.hostName, country: room.hostCountry }}
                        showCode={false}
                        showFlag={true}
                        className={styles.hostName}
                      />
                      {` · ${label}`}
                    </div>
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
                            extra={
                              match.sd === "differs" &&
                              cardCheck &&
                              cardCheck !== "checking" &&
                              cardCheck.kind === "differs"
                                ? Messages.megabytes(cardCheck.bytes)
                                : undefined
                            }
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
                        {/* The host's codes, which the room runs for everyone (room-codes.md). */}
                        {room.build.codes && room.build.codes.length > 0 && <CodesChip names={room.build.codes} />}
                      </div>
                    )}
                    {!own && (
                      <JoinsWith
                        match={match}
                        anchorRef={(el) => {
                          if (el) {
                            anchors.current.set(room.code, el);
                          } else {
                            anchors.current.delete(room.code);
                          }
                        }}
                        onOpen={(anchor) => {
                          if (!off) {
                            openMenu(room, match, anchor, false);
                          }
                        }}
                      />
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
  const menuRoom = menu ? listed.find((r) => r.code === menu.code) ?? null : null;
  const menuMatch = menuRoom ? matchOf(menuRoom) : null;
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
      content={
        <div className={styles.room}>
          {content}
          <JoinWithMenu
            anchor={menuRoom ? menu!.anchor : null}
            room={menuRoom?.build ?? null}
            match={menuMatch}
            checks={checks}
            onClose={() => setMenu(null)}
            onPick={(picked, remember) => {
              const joinAfter = menu?.joinAfter;
              setMenu(null);
              if (!menuRoom) {
                return;
              }
              setPickedHere((p) => ({ ...p, [menuRoom.code]: picked.id }));
              if (remember) {
                onSetRoomGame?.(menuRoom.build.hash, picked.id);
              }
              if (joinAfter && !disabled) {
                onJoin(picked, menuRoom.code);
              }
            }}
          />
        </div>
      }
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
  onCodes,
  onHost,
}: {
  build: ModBuild;
  /** Hashing: the share done, null while not known; undefined when not hashing. */
  progress: number | null | undefined;
  starting: boolean;
  disabled: boolean;
  onEdit: () => void;
  onRemove: () => void;
  onCodes?: () => void;
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
          {build.builtIn
            ? Messages.parts(fileName(build.discPath)!, Messages.builtInFiles(), null)
            : Messages.parts(fileName(build.discPath)!, fileName(build.launcherPath), fileName(build.sdPath))}
        </div>
        <Tooltip title={build.builtIn ? Messages.editBuiltInTooltip() : Messages.editTooltip()}>
          <span>
            <IconButton size="small" aria-label={Messages.edit()} onClick={onEdit} disabled={disabled || hashing}>
              <EditOutlinedIcon fontSize="small" />
            </IconButton>
          </span>
        </Tooltip>
        {!build.builtIn && (
          <Tooltip title={Messages.removeTooltip()}>
            <span>
              <IconButton size="small" aria-label={Messages.remove()} onClick={onRemove} disabled={disabled || hashing}>
                <DeleteOutlineRoundedIcon fontSize="small" />
              </IconButton>
            </span>
          </Tooltip>
        )}
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
        {/* The codes its rooms run. Not for a mod booted by its launcher (P+'s too): the launcher puts
            its own Gecko code handler where Dolphin's goes, and the mod's codes are in it. */}
        {onCodes && hasRoomCodes(build) && (
          <CodesButton names={(build.codes?.on ?? []).map((c) => c.name)} onClick={onCodes} />
        )}
        <Tooltip title={Messages.publicRoomTooltip()}>
          <label className={styles.toggle}>
            <Switch size="small" checked={isPublic} onChange={(e) => setIsPublic(e.target.checked)} />
            <PublicLabel isPublic={isPublic} />
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

/** The public switch's label: "Public" on, "Private" off (bug report #282). Both words share one
 *  grid cell, the other one hidden, so the label is as wide as the longer word in any theme's font
 *  and the switch and Host don't move when it flips. */
const PublicLabel = ({ isPublic }: { isPublic: boolean }) => (
  <span className={styles.toggleText}>
    <span className={clsx(!isPublic && styles.toggleTextOff)} aria-hidden={!isPublic}>
      {Messages.publicRoom()}
    </span>
    <span className={clsx(isPublic && styles.toggleTextOff)} aria-hidden={isPublic}>
      {Messages.privateRoom()}
    </span>
  </span>
);

const RoomPanel = ({
  status,
  onRequest,
  onCopyCode,
  yourRoom,
  onRename,
  onAdminRename,
  onCodes,
}: {
  status: ModRoomStatus | null;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
  yourRoom?: YourRoom | null;
  onRename?: (code: string, name: string) => Promise<void>;
  /** An admin account: the pencil even when another player hosts the room. */
  onAdminRename?: (code: string, name: string) => Promise<void>;
  /** The host: the game's Codes dialog (locked while the room is open). */
  onCodes?: () => void;
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
  let title: ReactNode = Messages.yourRoom();
  if (room) {
    // The room's name (the server's, once it lists this room), the host's pencil next to it.
    const hostName = room.slots.find((s) => s.slot === room.host)?.player?.displayName;
    const listed = yourRoom?.kind === "mods" && yourRoom.code === room.code ? yourRoom : null;
    title = (
      <RoomNameField
        name={listed?.name ?? null}
        fallback={hostName ? RoomNameMessages.defaultName(hostName) : Messages.inRoom(room.code)}
        canEdit={!!listed && ((isHost && !!onRename) || !!onAdminRename)}
        onSave={async (name) => (isHost && onRename ? onRename(room.code, name) : onAdminRename?.(room.code, name))}
      />
    );
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
              <button
                type="button"
                className={styles.codeChip}
                aria-label={Messages.copyCode()}
                onClick={() => onCopyCode(room.code)}
              >
                {room.code}
                <ContentCopyRoundedIcon style={{ fontSize: 13 }} />
              </button>
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
                  <PublicLabel isPublic={publicClicked ?? room.public} />
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
              <div className={styles.roomBuild}>
                {Messages.buildName(room.build.name, room.build.hash.slice(0, 8))}
                {room.build.codes && room.build.codes.length > 0 && (
                  <CodesChip names={room.build.codes} onClick={isHost ? onCodes : undefined} />
                )}
              </div>
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
