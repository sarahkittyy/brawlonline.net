import { stageInfo } from "@common/stages";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import CloudOffOutlinedIcon from "@mui/icons-material/CloudOffOutlined";
import LiveTvOutlinedIcon from "@mui/icons-material/LiveTvOutlined";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import VisibilityOutlinedIcon from "@mui/icons-material/VisibilityOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import FormControlLabel from "@mui/material/FormControlLabel";
import Switch from "@mui/material/Switch";
import Tooltip from "@mui/material/Tooltip";
import type { LiveGameEntry, LiveMode, LivePlayer, SpectateState } from "@spectate/types";
import { clsx } from "clsx";
import React from "react";

import { getCharacterIcon } from "@/lib/utils";

import { ContentBlock } from "../overview/content_block/content_block";
import { SpectateMessages as Messages } from "./spectate.messages";
import styles from "./spectate.module.css";

export type SpectateListState =
  | { kind: "logged-out" }
  | { kind: "loading" }
  | { kind: "unreachable" }
  | { kind: "ready"; games: LiveGameEntry[] };

export type SpectateViewProps = {
  state: SpectateListState;
  /** The player's uid: their own games can't be watched. */
  uid?: string;
  /** What the launcher is watching. */
  watching?: Pick<SpectateState, "status" | "stream" | "waitingForGame">;
  /** The stream whose watch is starting (its row shows a spinner). */
  startingId?: string;
  autoPlay: boolean;
  onAutoPlayChange: (on: boolean) => void;
  onWatch: (game: LiveGameEntry) => void;
  onRetry: () => void;
  onLogIn: () => void;
  /** The time now (ms): each row's game time is counted from its start. */
  now: number;
  /** Games that appeared since the tab opened: they slide in and pulse once. */
  freshIds?: ReadonlySet<string>;
};

/** Newest set first (docs/design/spectating.md §6.1): a set's next game doesn't move its row. */
export function sortLiveGames(games: readonly LiveGameEntry[]): LiveGameEntry[] {
  return [...games].sort((a, b) => Date.parse(b.startedAt) - Date.parse(a.startedAt));
}

/** "4:05": how long the current game has been going. */
export function gameClock(startedAt: string, now: number): string {
  const secs = Math.max(0, Math.floor((now - Date.parse(startedAt)) / 1000));
  return `${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, "0")}`;
}

/** The set's score ("1-0"), or null before a game was won. Teams: per team. */
export function setScore(game: LiveGameEntry): string | null {
  const sides = sidesOf(game);
  const wins = sides.map((side) => Math.max(...side.map((p) => p.wins)));
  return wins.some((w) => w > 0) ? wins.join("-") : null;
}

/** The players by side: a team per side in a team game, else one player each. */
function sidesOf(game: LiveGameEntry): LivePlayer[][] {
  if (!game.isTeams) {
    return game.players.map((p) => [p]);
  }
  const teams = new Map<number | null, LivePlayer[]>();
  for (const p of game.players) {
    const t = teams.get(p.teamId) ?? [];
    t.push(p);
    teams.set(p.teamId, t);
  }
  return [...teams.values()];
}

const modeLabel = (mode: LiveMode): string => {
  switch (mode) {
    case "ranked":
      return Messages.modeRanked();
    case "unranked":
      return Messages.modeUnranked();
    case "direct":
      return Messages.modeDirect();
    case "room":
      return Messages.modeRoom();
  }
};

/**
 * Home > Spectate (docs/design/spectating.md §6.1): every live game, newest first, with who plays, how long the game
 * has been going and how many watch it. A click watches it in the playback Dolphin (switching what it plays).
 */
export const SpectateView = ({
  state,
  uid,
  watching,
  startingId,
  autoPlay,
  onAutoPlayChange,
  onWatch,
  onRetry,
  onLogIn,
  now,
  freshIds,
}: SpectateViewProps) => {
  const status = watchingStatus(watching);
  return (
    <div className={styles.block}>
      <ContentBlock
        fill={true}
        title={Messages.liveGames()}
        titleAction={
          state.kind !== "logged-out" ? (
            <Tooltip title={Messages.autoPlayTooltip()} placement="top" enterDelay={400} disableInteractive={true}>
              <FormControlLabel
                className={styles.autoPlay}
                sx={{ "& .MuiFormControlLabel-label": { fontSize: 13 } }}
                control={
                  <Switch
                    size="small"
                    color="secondary"
                    checked={autoPlay}
                    onChange={(e) => onAutoPlayChange(e.target.checked)}
                    inputProps={{ "aria-label": Messages.autoPlay() }}
                    data-testid="auto-play"
                  />
                }
                label={Messages.autoPlay()}
              />
            </Tooltip>
          ) : undefined
        }
        endIcon={status ? <div className={styles.watchStatus}>{status}</div> : undefined}
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
      case "ready": {
        if (state.games.length === 0) {
          return (
            <div className={styles.centered}>
              <LiveTvOutlinedIcon style={{ fontSize: 48 }} />
              <div>
                <p>{Messages.noGames()}</p>
                <p>{Messages.noGamesHint()}</p>
              </div>
            </div>
          );
        }
        const watchedId =
          watching && watching.status !== "idle" && watching.status !== "error" ? watching.stream : null;
        return (
          <ul className={styles.list}>
            {sortLiveGames(state.games).map((game) => (
              <li key={game.id}>
                <LiveGameRow
                  game={game}
                  now={now}
                  own={uid != null && game.players.some((p) => p.uid === uid)}
                  watching={watchedId === game.id}
                  starting={startingId === game.id}
                  busy={startingId != null}
                  fresh={freshIds?.has(game.id) ?? false}
                  onWatch={onWatch}
                />
              </li>
            ))}
          </ul>
        );
      }
    }
  }
};

function watchingStatus(watching: SpectateViewProps["watching"]): string | null {
  if (!watching) {
    return null;
  }
  if (watching.waitingForGame) {
    return Messages.statusWaiting();
  }
  switch (watching.status) {
    case "connecting":
      return Messages.statusConnecting();
    case "reconnecting":
      return Messages.statusReconnecting();
    default:
      return null;
  }
}

export const LiveGameRow = React.memo(function LiveGameRow({
  game,
  now,
  own,
  watching,
  starting,
  busy,
  fresh,
  onWatch,
}: {
  game: LiveGameEntry;
  now: number;
  /** The player plays in it: no Watch. */
  own: boolean;
  /** It is the game being watched. */
  watching: boolean;
  starting: boolean;
  /** Another watch is starting. */
  busy: boolean;
  fresh: boolean;
  onWatch: (game: LiveGameEntry) => void;
}) {
  const over = game.game.state === "over";
  const disabled = own || over;
  const tooltip = own ? Messages.yourGameTooltip() : over ? Messages.endedTooltip() : Messages.watchTooltip();
  const sides = sidesOf(game);
  const stage = stageInfo(game.game.stageId)?.name;
  const time =
    game.game.state === "playing"
      ? gameClock(game.game.startedAt, now)
      : game.game.state === "between"
      ? Messages.betweenGames()
      : Messages.ended();
  const spectators = game.spectators ?? 0;

  return (
    <Tooltip title={tooltip} placement="top" enterDelay={600} disableInteractive={true}>
      <button
        type="button"
        className={clsx(styles.row, disabled && styles.disabled, watching && styles.watchingRow, fresh && styles.fresh)}
        aria-disabled={disabled || busy}
        data-stream={game.id}
        onClick={() => {
          if (!disabled && !busy && !watching) {
            onWatch(game);
          }
        }}
      >
        <div className={styles.players}>
          {sides.map((side, i) => (
            <React.Fragment key={i}>
              {i > 0 && <span className={styles.vs}>{Messages.vs()}</span>}
              <span className={styles.side}>
                {side.map((p, j) => (
                  <React.Fragment key={p.uid}>
                    {j > 0 && <span className={styles.vs}>{Messages.and()}</span>}
                    <span className={styles.player} title={p.connectCode}>
                      <img className={styles.stock} src={getCharacterIcon(p.characterId, p.costume)} alt="" />
                      <span className={styles.name}>{p.displayName}</span>
                    </span>
                  </React.Fragment>
                ))}
              </span>
            </React.Fragment>
          ))}
        </div>
        <span className={styles.mode}>{modeLabel(game.mode)}</span>
        <div className={styles.game}>
          <div>{Messages.gameScore(game.game.number, setScore(game))}</div>
          {stage && <div className={styles.stage}>{stage}</div>}
        </div>
        <span className={clsx(styles.time, game.game.state === "playing" && styles.live)} data-testid="game-time">
          {game.game.state === "playing" && <span className={styles.liveDot} />}
          {time}
        </span>
        <Tooltip title={Messages.spectators(spectators)} placement="top" disableInteractive={true}>
          <span className={styles.eye} data-testid="spectators">
            <VisibilityOutlinedIcon style={{ fontSize: 16 }} />
            {spectators}
          </span>
        </Tooltip>
        <span className={styles.action}>
          {starting ? (
            <CircularProgress color="inherit" size={14} />
          ) : own ? (
            Messages.yourGame()
          ) : watching ? (
            Messages.watching()
          ) : over ? (
            Messages.ended()
          ) : (
            <>
              {Messages.watch()}
              <ChevronRightIcon style={{ fontSize: 18 }} />
            </>
          )}
        </span>
      </button>
    </Tooltip>
  );
});
