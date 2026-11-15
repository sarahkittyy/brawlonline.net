import { stageInfo } from "@common/stages";
import ChevronRightIcon from "@mui/icons-material/ChevronRight";
import CloseIcon from "@mui/icons-material/Close";
import CloudOffOutlinedIcon from "@mui/icons-material/CloudOffOutlined";
import LiveTvOutlinedIcon from "@mui/icons-material/LiveTvOutlined";
import LockOutlinedIcon from "@mui/icons-material/LockOutlined";
import SearchIcon from "@mui/icons-material/Search";
import SearchOffOutlinedIcon from "@mui/icons-material/SearchOffOutlined";
import VisibilityOutlinedIcon from "@mui/icons-material/VisibilityOutlined";
import Button from "@mui/material/Button";
import CircularProgress from "@mui/material/CircularProgress";
import FormControlLabel from "@mui/material/FormControlLabel";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import InputBase from "@mui/material/InputBase";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import Switch from "@mui/material/Switch";
import type { TooltipProps } from "@mui/material/Tooltip";
import Tooltip from "@mui/material/Tooltip";
import type { LiveGameEntry, LiveMode, LivePlayer, SpectateState } from "@spectate/types";
import { clsx } from "clsx";
import React, { useEffect, useLayoutEffect, useRef, useState } from "react";

import { ShellMessages } from "@/app/shell/shell.messages";
import { MiniProfileTooltip } from "@/components/profile/mini_profile";
import { usePlayerMenuOpener } from "@/lib/social/use_player_menu";
import { getCharacterIcon } from "@/lib/utils";
import { useStageIcon } from "@/pages/rulesets/stages_section";

import { SpectateMessages as Messages } from "./spectate.messages";
import styles from "./spectate.module.css";

export type SpectateListState =
  | { kind: "logged-out" }
  | { kind: "loading" }
  | { kind: "unreachable" }
  | { kind: "ready"; games: LiveGameEntry[] };

/** The list's order: by the game clock (newest first), or by the players' Elo (highest first). */
export type LiveSort = "newest" | "elo";

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
  sort?: LiveSort;
  onSortChange?: (sort: LiveSort) => void;
  /** The highest-rated game above the list, out of it (the Spectate page; off in tests of the list). */
  showFeatured?: boolean;
};

/**
 * By how long the game being played has been going, as each row's clock shows it, newest first (the user,
 * 2026-10-15: sorting by the set's start left the clocks out of order). Games between a set's games come after
 * the games being played, and ended streams at the bottom (they can still be watched from the server for a while).
 */
export function sortLiveGames(games: readonly LiveGameEntry[], sort: LiveSort = "newest"): LiveGameEntry[] {
  const group = (g: LiveGameEntry) => (g.game.state === "playing" ? 0 : g.game.state === "between" ? 1 : 2);
  const newest = (a: LiveGameEntry, b: LiveGameEntry) =>
    group(a) - group(b) || Date.parse(b.game.startedAt) - Date.parse(a.game.startedAt);
  if (sort === "newest") {
    return [...games].sort(newest);
  }
  // By Elo (the user, 2026-10-24): the highest first, games without a placed player after them, ties as above. Ended
  // streams stay at the bottom; a set between its games is still the same set, so it keeps its place.
  const ended = (g: LiveGameEntry) => (g.game.state === "over" ? 1 : 0);
  const elo = (g: LiveGameEntry) => gameElo(g) ?? -Infinity;
  return [...games].sort((a, b) => ended(a) - ended(b) || elo(b) - elo(a) || newest(a, b));
}

/** A game's Elo: the average rating of its placed players (a player still placing has none), or null without one. */
export function gameElo(game: LiveGameEntry): number | null {
  const ratings = game.players.map((p) => p.rating).filter((r): r is number => typeof r === "number");
  return ratings.length > 0 ? ratings.reduce((a, b) => a + b, 0) / ratings.length : null;
}

/** The games with a player whose name or connect code has `search` in it (any case); all of them without one. */
export function searchLiveGames(games: readonly LiveGameEntry[], search: string): readonly LiveGameEntry[] {
  const query = search.trim().toLowerCase();
  if (!query) {
    return games;
  }
  return games.filter((g) =>
    g.players.some((p) => p.displayName.toLowerCase().includes(query) || p.connectCode.toLowerCase().includes(query)),
  );
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
    case "twos":
      return Messages.modeTwos();
  }
};

/**
 * Home > Spectate (docs/design/spectating.md §6.1): every live game, newest first (or by Elo), with who plays, how long
 * the game has been going and how many watch it. A click watches it in the playback Dolphin (switching what it plays).
 * The search above the list keeps the games with a player whose name or code has it.
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
  sort = "newest",
  onSortChange,
  showFeatured = false,
}: SpectateViewProps) => {
  const status = watchingStatus(watching);
  const [search, setSearch] = useState("");
  const featured =
    showFeatured && state.kind === "ready" && search.trim() === "" ? pickFeatured(state.games, uid) : null;
  const watchedId = watching && watching.status !== "idle" && watching.status !== "error" ? watching.stream : null;
  return (
    <div className={styles.block}>
      <div className={styles.head}>
        <h1 className={styles.title}>{ShellMessages.spectate()}</h1>
        {status && <div className={styles.watchStatus}>{status}</div>}
        <span className={styles.grow} />
        {state.kind !== "logged-out" && (
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
        )}
      </div>
      {featured && (
        <FeaturedGame
          game={featured}
          now={now}
          watching={watchedId === featured.id}
          starting={startingId === featured.id}
          busy={startingId != null}
          onWatch={onWatch}
        />
      )}
      <div className={styles.bar}>
        <h2 className={styles.heading}>{Messages.liveGames()}</h2>
        {state.kind === "ready" && renderListControls()}
      </div>
      <div className={styles.body}>{renderBody(featured?.id)}</div>
    </div>
  );

  function renderListControls() {
    return (
      <>
        <InputBase
          className={styles.search}
          placeholder={Messages.search()}
          value={search}
          onChange={(e) => setSearch(e.target.value)}
          inputProps={{ "aria-label": Messages.search(), "data-testid": "search" }}
          endAdornment={
            <InputAdornment position="end">
              <IconButton
                size="small"
                onClick={() => setSearch("")}
                disabled={search.length === 0}
                aria-label={Messages.clearSearch()}
              >
                {search.length > 0 ? <CloseIcon fontSize="inherit" /> : <SearchIcon fontSize="inherit" />}
              </IconButton>
            </InputAdornment>
          }
        />
        <Select
          size="small"
          value={sort}
          onChange={(e) => onSortChange?.(e.target.value as LiveSort)}
          className={styles.sort}
          inputProps={{ "aria-label": Messages.sort(), "data-testid": "sort" }}
        >
          <MenuItem value="newest">{Messages.sortNewest()}</MenuItem>
          <MenuItem value="elo">{Messages.sortElo()}</MenuItem>
        </Select>
      </>
    );
  }

  function renderBody(featuredId?: string) {
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
        const shown = searchLiveGames(state.games, search).filter((g) => g.id !== featuredId);
        if (shown.length === 0 && featuredId) {
          return null;
        }
        if (shown.length === 0) {
          return (
            <div className={styles.centered}>
              <SearchOffOutlinedIcon style={{ fontSize: 48 }} />
              <p>{Messages.noMatch(search.trim())}</p>
            </div>
          );
        }
        return (
          <ul className={styles.list}>
            {sortLiveGames(shown, sort).map((game) => (
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

/**
 * A row's tooltip, closed when anything scrolls (bug reports #179/#180): the tooltip lives in the body and follows its
 * row, so a wheel over the list carried it over the header and past the window's edge, where it scrolled the whole
 * page. Hovering again after the scroll opens it for the row under the cursor.
 */
function RowTooltip(props: Omit<TooltipProps, "open" | "onOpen" | "onClose">) {
  const [open, setOpen] = useState(false);
  useEffect(() => {
    if (!open) {
      return;
    }
    // Scroll events don't bubble: caught on the way down, any scrolled element (the list's or the page's) closes it.
    const close = () => setOpen(false);
    window.addEventListener("scroll", close, { capture: true, passive: true });
    return () => window.removeEventListener("scroll", close, { capture: true });
  }, [open]);
  return <Tooltip {...props} open={open} onOpen={() => setOpen(true)} onClose={() => setOpen(false)} />;
}

/** A live game's player: their hover card, and the player menu on a right click. */
const LivePlayerName = ({ player }: { player: LivePlayer }) => {
  const onContextMenu = usePlayerMenuOpener(player);
  return (
    <MiniProfileTooltip uid={player.uid}>
      <span className={styles.name} onContextMenu={onContextMenu}>
        {player.displayName}
      </span>
    </MiniProfileTooltip>
  );
};

/**
 * Who plays, in one of three shapes only (Sarah's board): all on one line; the first side on one line and "vs" with
 * the second side on the next; or, when a side doesn't fit on a line, each player on a line of their own ("vs" before
 * the second side's first). Never a side half wrapped beside one that isn't: the row measures its widest side.
 */
const SidesCell = ({ sides }: { sides: LivePlayer[][] }) => {
  const ref = useRef<HTMLDivElement>(null);
  const [stacked, setStacked] = useState(false);
  const who = sides.map((side) => side.map((p) => `${p.uid}:${p.displayName}`).join("&")).join("|");
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) {
      return;
    }
    const check = () => {
      // Each side laid out on one line for a moment, measured, before paint.
      const was = el.dataset.stacked;
      delete el.dataset.stacked;
      const widest = Math.max(0, ...Array.from(el.children).map((c) => (c as HTMLElement).scrollWidth));
      if (was !== undefined) {
        el.dataset.stacked = was;
      }
      setStacked(widest > el.clientWidth + 0.5);
    };
    check();
    if (typeof ResizeObserver === "undefined") {
      return;
    }
    const observer = new ResizeObserver(check);
    observer.observe(el);
    return () => observer.disconnect();
    // Measured again when the players change (not on each clock tick's new array).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [who]);
  return (
    <div ref={ref} className={styles.players} data-stacked={stacked ? "" : undefined}>
      {sides.map((side, i) => (
        <span key={i} className={styles.side}>
          {side.map((p, j) => (
            <span key={p.uid} className={styles.player}>
              {i > 0 && j === 0 && <span className={styles.vs}>{Messages.vs()}</span>}
              <img className={styles.stock} src={getCharacterIcon(p.characterId, p.costume)} alt="" />
              <LivePlayerName player={p} />
              {j < side.length - 1 && <span className={styles.vs}>{Messages.and()}</span>}
            </span>
          ))}
        </span>
      ))}
    </div>
  );
};

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
  const disabled = own;
  const tooltip = own ? Messages.yourGameTooltip() : over ? Messages.watchEndedTooltip() : Messages.watchTooltip();
  const sides = sidesOf(game);
  const stage = stageInfo(game.game.stageId)?.name;
  const time =
    game.game.state === "playing"
      ? gameClock(game.game.startedAt, now)
      : game.game.state === "between"
      ? Messages.betweenGames()
      : Messages.ended();
  const spectators = game.spectators ?? 0;
  const stageIcon = useStageIcon()(game.game.stageId);

  return (
    <RowTooltip title={tooltip} placement="top" enterDelay={600} disableInteractive={true}>
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
        {stageIcon ? (
          <img className={styles.stageThumb} src={stageIcon} alt="" />
        ) : (
          <span className={styles.stageThumb} />
        )}
        <SidesCell sides={sides} />
        <div className={styles.game}>
          <div className={styles.gameLine}>
            {modeLabel(game.mode)} · {Messages.gameScore(game.game.number, setScore(game))}
          </div>
          {stage && <div className={styles.stage}>{stage}</div>}
        </div>
        <span
          className={clsx(styles.time, game.game.state === "playing" && styles.live, over && styles.ended)}
          data-testid="game-time"
        >
          {game.game.state === "playing" && <span className={styles.liveDot} />}
          {time}
        </span>
        <RowTooltip title={Messages.spectators(spectators)} placement="top" disableInteractive={true}>
          <span className={styles.eye} data-testid="spectators">
            <VisibilityOutlinedIcon style={{ fontSize: 16 }} />
            {spectators}
          </span>
        </RowTooltip>
        <span className={styles.action}>
          {starting ? (
            <CircularProgress color="inherit" size={14} />
          ) : own ? (
            Messages.yourGame()
          ) : watching ? (
            Messages.watching()
          ) : (
            <>
              {Messages.watch()}
              <ChevronRightIcon style={{ fontSize: 18 }} />
            </>
          )}
        </span>
      </button>
    </RowTooltip>
  );
});

/**
 * The game shown above the list: the highest-rated one the player can watch (the newest without ratings). A 1v1 comes
 * before any 2v2 or free-for-all, whatever their ratings (Sarah's board, 2026-11-14).
 */
export function pickFeatured(games: readonly LiveGameEntry[], uid?: string): LiveGameEntry | null {
  const watchable = games.filter((g) => g.game.state !== "over" && !(uid && g.players.some((p) => p.uid === uid)));
  const singles = watchable.filter((g) => !g.isTeams && g.players.length === 2);
  return sortLiveGames(singles.length > 0 ? singles : watchable, "elo")[0] ?? null;
}

/**
 * The game above the list (docs/design/launcher-redesign.md §7): its stage, the clock, mode, game and score and the
 * players watching, then each side as [character] [name] vs [name] [character], and Watch. When the players wrap onto
 * more lines, every side reads [character] [name] (Sarah's board): mirrored only while all are on one line.
 */
const FeaturedGame = ({
  game,
  now,
  watching,
  starting,
  busy,
  onWatch,
}: {
  game: LiveGameEntry;
  now: number;
  watching: boolean;
  starting: boolean;
  busy: boolean;
  onWatch: (game: LiveGameEntry) => void;
}) => {
  const stageIcon = useStageIcon()(game.game.stageId);
  const sides = sidesOf(game);
  const playing = game.game.state === "playing";
  const playersRef = useRef<HTMLDivElement>(null);
  const [wrapped, setWrapped] = useState(false);
  const who = sides.map((side) => side.map((p) => `${p.uid}:${p.displayName}`).join("&")).join("|");
  useLayoutEffect(() => {
    const el = playersRef.current;
    if (!el) {
      return;
    }
    // Wrapped when the last side sits below the first. Mirroring doesn't change a side's width, so no flip-flop.
    const check = () => {
      const first = el.firstElementChild as HTMLElement | null;
      const last = el.lastElementChild as HTMLElement | null;
      setWrapped(!!first && !!last && last.offsetTop > first.offsetTop + first.offsetHeight / 2);
    };
    check();
    if (typeof ResizeObserver === "undefined") {
      return;
    }
    const observer = new ResizeObserver(check);
    observer.observe(el);
    return () => observer.disconnect();
    // Measured again when the players change (not on each clock tick's new array).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [who]);
  const person = (p: LivePlayer, mirrored: boolean) => {
    const parts = [
      <img key="stock" className={styles.featuredStock} src={getCharacterIcon(p.characterId, p.costume)} alt="" />,
      <LivePlayerName key="name" player={p} />,
    ];
    return <span className={styles.featuredPlayer}>{mirrored ? parts.reverse() : parts}</span>;
  };
  return (
    <section className={styles.featured} data-testid="featured-game" data-stream={game.id}>
      {stageIcon && <img className={styles.featuredStage} src={stageIcon} alt="" />}
      <div className={styles.featuredText}>
        <div className={styles.featuredLine}>
          <span className={clsx(styles.time, playing && styles.live)}>
            {playing && <span className={styles.liveDot} />}
            {playing ? gameClock(game.game.startedAt, now) : Messages.betweenGames()}
          </span>
          <span className={styles.mode}>{modeLabel(game.mode)}</span>
          <span>{Messages.gameScore(game.game.number, setScore(game))}</span>
          <span className={styles.eye}>
            <VisibilityOutlinedIcon style={{ fontSize: 16 }} />
            {game.spectators ?? 0}
          </span>
        </div>
        <div
          ref={playersRef}
          className={styles.featuredPlayers}
          data-testid="featured-players"
          data-wrapped={wrapped ? "" : undefined}
        >
          {sides.map((side, i) => (
            <React.Fragment key={i}>
              {i > 0 && <span className={styles.vs}>{Messages.vs()}</span>}
              <span className={clsx(styles.featuredSide, i === 0 ? styles.sideP1 : styles.sideP2)}>
                {side.map((p, j) => (
                  <React.Fragment key={p.uid}>
                    {j > 0 && <span className={styles.vs}>{Messages.and()}</span>}
                    {person(p, i > 0 && !wrapped)}
                  </React.Fragment>
                ))}
              </span>
            </React.Fragment>
          ))}
        </div>
      </div>
      <Button
        variant="contained"
        className={styles.featuredWatch}
        disabled={busy || watching}
        onClick={() => onWatch(game)}
        startIcon={starting ? <CircularProgress color="inherit" size={16} /> : undefined}
        data-testid="featured-watch"
      >
        {watching ? Messages.watching() : Messages.watch()}
      </Button>
    </section>
  );
};
