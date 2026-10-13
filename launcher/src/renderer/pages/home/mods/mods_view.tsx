import type {
  ModBuild,
  ModRoomRequest,
  ModRoomSlot,
  ModRoomStatus,
  ModsController,
  ModsState,
} from "@dolphin/mods/types";
import AddRoundedIcon from "@mui/icons-material/AddRounded";
import ContentCopyRoundedIcon from "@mui/icons-material/ContentCopyRounded";
import DeleteOutlineRoundedIcon from "@mui/icons-material/DeleteOutlineRounded";
import Button from "@mui/material/Button";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import Tooltip from "@mui/material/Tooltip";
import { clsx } from "clsx";
import { useState } from "react";

import { ContentBlock } from "../overview/content_block/content_block";
import { ModsMessages as Messages } from "./mods.messages";
import styles from "./mods.module.css";

export type ModsViewProps = {
  /** null while it loads. */
  state: ModsState | null;
  /** Hashing progress (0-1) by build id. */
  hashProgress: Record<string, number>;
  /** A build whose host or join is starting Dolphin. */
  startingBuildId: string | null;
  error: string | null;
  controller: ModsController;
  onControllerChange: (controller: ModsController) => void;
  onAddBuild: () => void;
  onRemoveBuild: (build: ModBuild) => void;
  onHost: (build: ModBuild, isPublic: boolean) => void;
  onJoin: (build: ModBuild, code: string) => void;
  onRequest: (request: ModRoomRequest) => void;
  onCopyCode: (code: string) => void;
};

const fileName = (p: string | null) => (p ? p.split(/[\\/]/).pop() ?? p : null);

/** Home > Brawl Mods (docs/design/brawl-mods.md §8): the player's builds, and the room Dolphin runs. */
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
          <Button size="small" startIcon={<AddRoundedIcon />} onClick={props.onAddBuild}>
            {Messages.addBuild()}
          </Button>
        }
        content={
          <div className={styles.builds}>
            <div className={styles.controllerRow}>
              <span>{Messages.controller()}</span>
              <Select
                size="small"
                value={props.controller}
                onChange={(e) => props.onControllerChange(e.target.value as ModsController)}
              >
                <MenuItem value="gc">{Messages.gameCube()}</MenuItem>
                <MenuItem value="wiimote">{Messages.wiiRemote()}</MenuItem>
              </Select>
            </div>
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
                onJoin={(code) => props.onJoin(build, code)}
              />
            ))}
          </div>
        }
      />
    </div>
  );
};

const BuildRow = ({
  build,
  progress,
  starting,
  disabled,
  onRemove,
  onHost,
  onJoin,
}: {
  build: ModBuild;
  progress: number | undefined;
  starting: boolean;
  disabled: boolean;
  onRemove: () => void;
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
