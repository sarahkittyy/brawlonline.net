/**
 * Highlights and L+R+Z clips (DESIGN.md §2): who on whom with their characters, nothing the
 * detector measured. A row plays in the playback Dolphin from just before the string to just after
 * it; ↑/↓ (or Tab) plays the next one at once, and the only wait (Dolphin's fast-forward to it)
 * shows with its progress. The list played from is the playback Dolphin's "<< < > >>"
 * (highlight_playlist.ts), and the row playing follows them. A click only selects the row (Ctrl+click
 * adds or removes one, Shift+click a range), a double click plays it; the selected rows render to
 * video (docs/design/highlights.md §4): Render (w/ Music) or Render (No Music), a file each, or one combo movie of them all.
 * `HighlightList` is a replay's (its stats page); `HighlightRows` lists rows of any replays (the
 * Replays > Highlights tab's, all of them). A row (docs/design/highlights.md §10) is the stage (rows of
 * many games), the players, and under them when in the game it plays and the game's stage and date;
 * at its end, shown when pointed at, selected or playing, the star (favorites.ts; always once
 * starred) and Play. Manual clips (the seek bar's, manual_clips.ts) come last, each with a button
 * that removes it.
 */
import { stageInfo } from "@common/stages";
import type { ReplayPlaybackStatus } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import CloseIcon from "@mui/icons-material/Close";
import DeleteOutlineIcon from "@mui/icons-material/DeleteOutline";
import MovieIcon from "@mui/icons-material/Movie";
import MusicOffIcon from "@mui/icons-material/MusicOff";
import PlayArrowRoundedIcon from "@mui/icons-material/PlayArrowRounded";
import StarIcon from "@mui/icons-material/Star";
import StarBorderIcon from "@mui/icons-material/StarBorder";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import FormControlLabel from "@mui/material/FormControlLabel";
import IconButton from "@mui/material/IconButton";
import LinearProgress from "@mui/material/LinearProgress";
import Tooltip from "@mui/material/Tooltip";
import type {
  HighlightPlayer,
  ReplayClip,
  ReplayHighlight,
  ReplayHighlights,
  ReplayManualClip,
  StockChange,
} from "@replays/highlights/types";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useMousetrap } from "@/lib/hooks/use_mousetrap";
import { useSetting } from "@/lib/hooks/use_settings";
import { getColor } from "@/lib/player_colors";
import { convertFrameCountToDurationString, monthDayHourFormat } from "@/lib/time";
import { getCharacterIcon } from "@/lib/utils";
import { useStageIcon } from "@/pages/rulesets/stages_section";
import { useServices } from "@/services";

import { favoriteKey, useFavoriteHighlights } from "./favorites";
import { HighlightListMessages as Messages } from "./highlight_list.messages";
import styles from "./highlight_list.module.css";
import type { PlaylistEntry } from "./highlight_nav";
import { setPlaylistEntries, usePlaylistStore } from "./highlight_nav";
import { useHighlightPlayer } from "./highlight_playlist";
import { manualClipEntry, useManualClips } from "./manual_clips";
import { RenderJobs, useRenderJobs } from "./render_jobs";
import { clipVideoName, comboVideoName, multiGameComboName } from "./render_names";
import { ReplayVersionNote } from "./replay_version_note";
import { useWindowedRows } from "./windowed_rows";

type Row = ReplayHighlight | ReplayClip | ReplayManualClip;

/** Starting for this long: the banner says where to look. */
const STARTING_HINT_MS = 20_000;

/** More stocks than this show as a count, not pips. */
const MAX_PIPS = 5;

/** A row: which replay it is of (its players, when it was played) and what of it. */
export type HighlightEntry = {
  key: string;
  path: string;
  game: ReplayHighlights;
  row: Row;
  /** When the game was played (ISO): shown on the row when the list has several games. */
  date?: string;
};

/** A replay's highlights and clips, on its stats page. */
export const HighlightList = ({ filePath, pplusRelease }: { filePath: string; pplusRelease?: string }) => {
  const { replayService } = useServices();
  const [detectionOff] = useSetting("disableHighlightDetection");
  const [clipOnLrz] = useSetting("clipOnLrz");
  const [clipHotkeys] = useSetting("clipHotkeys");
  const query = useQuery({
    queryKey: ["replayHighlights", filePath, detectionOff, clipOnLrz, clipHotkeys],
    queryFn: () => replayService.getHighlights(filePath),
  });
  const data = query.data;
  const { clips: manualClips } = useManualClips();
  // (A replay without highlight data, e.g. a spectated game kept for a clip, lists its manual clips only.)
  const entries: HighlightEntry[] = useMemo(
    () =>
      data
        ? [
            ...(data.error ? [] : [...data.highlights, ...data.clips]).map((row, i) => ({
              key: `${i}`,
              path: filePath,
              game: data,
              row,
            })),
            ...manualClips
              .filter((c) => c.path === filePath)
              .sort((a, b) => a.startFrame - b.startFrame)
              .map((c) => manualClipEntry(c, data)),
          ]
        : [],
    [data, filePath, manualClips],
  );

  if (query.isLoading) {
    return <div className={styles.note}>{Messages.finding()}</div>;
  }
  if (query.error) {
    return <div className={styles.note}>{Messages.couldNotLoad(String(query.error))}</div>;
  }
  if (!data) {
    return null;
  }
  return (
    <HighlightRows
      sourceId={`stats:${filePath}`}
      entries={entries}
      resetKey={filePath}
      versionNote={<ReplayVersionNote release={pplusRelease} />}
      emptyNote={
        data.error
          ? Messages.noData()
          : detectionOff
          ? Messages.detectionOff()
          : data.highlights.length === 0
          ? Messages.noHighlights()
          : null
      }
      hideRows={entries.length === 0}
      comboName={(chosen) => comboVideoName(data.players, chosen.length, filePath)}
    />
  );
};

type HighlightRowsProps = {
  /** Which list this is (the Highlights tab, a replay's stats page): the playback Dolphin's buttons go by the one played from. */
  sourceId: string;
  /** Highlights, then L+R+Z clips, then manual clips (each kind after the first has a heading). */
  entries: HighlightEntry[];
  /** Changes when the list is another (a new replay): nothing of the last one's is kept. */
  resetKey: string;
  versionNote?: React.ReactNode;
  /** Said above the rows (none found, detection off...). */
  emptyNote?: string | null;
  hideRows?: boolean;
  /** Shows each row's game date (rows of many games). */
  showDates?: boolean;
  /** The list's top bar (the Highlights tab's search and sort); the render buttons are on its bottom one. */
  headerExtra?: React.ReactNode;
  comboName: (chosen: HighlightEntry[]) => string;
};

export const HighlightRows = ({
  sourceId,
  entries,
  resetKey,
  versionNote,
  emptyNote,
  hideRows,
  showDates,
  headerExtra,
  comboName,
}: HighlightRowsProps) => {
  const { dolphinService } = useServices();
  const { renderHighlights } = useDolphinActions(dolphinService);
  const playEntry = useHighlightPlayer();
  const { favorites, toggle: toggleFavorite } = useFavoriteHighlights();
  const { remove: removeManualClip } = useManualClips();

  const [selected, setSelected] = useState<number | null>(null);
  const [status, setStatus] = useState<ReplayPlaybackStatus | null>(null);
  const [dismissed, setDismissed] = useState<string | null>(null);
  // What the playback Dolphin plays, if from this list.
  const mine = usePlaylistStore((s) => s.source === sourceId);
  const current = usePlaylistStore((s) => s.current);
  const commandId = usePlaylistStore((s) => s.commandId);
  const request = mine && commandId && dismissed !== commandId ? commandId : null;
  const listRef = useRef<HTMLDivElement>(null);
  const headerRef = useRef<HTMLDivElement>(null);
  const footerRef = useRef<HTMLDivElement>(null);
  // The rows selected (to render), and the one a Shift+click's range starts from.
  const [ticked, setTicked] = useState<Set<string>>(new Set());
  const [anchor, setAnchor] = useState<string | null>(null);
  const [combo, setCombo] = useState(false);
  const [queuing, setQueuing] = useState(false);
  const allJobs = useRenderJobs();
  // This list's renders (a cancelled one is gone: the player did it).
  const paths = useMemo(() => new Set(entries.map((e) => e.path)), [entries]);
  const jobs = useMemo(
    () => allJobs.filter((j) => j.state !== "cancelled" && j.paths.some((p) => paths.has(p))),
    [allJobs, paths],
  );

  // Another list (the stats page's next/previous replay): nothing of the last one's.
  useEffect(() => {
    setSelected(null);
    setTicked(new Set());
    setAnchor(null);
    setCombo(false);
  }, [resetKey]);

  // The rows as the playback Dolphin's list: the one played from follows what this one shows
  // now (another section, another sort, more rows found).
  const playlist: PlaylistEntry[] = useMemo(
    () =>
      entries.map((e) => ({
        key: e.key,
        path: e.path,
        startFrame: e.row.startFrame,
        endFrame: e.row.endFrame,
      })),
    [entries],
  );
  useEffect(() => setPlaylistEntries(sourceId, playlist, dolphinService), [sourceId, playlist, dolphinService]);

  // Only what Dolphin says about the request playing (an older one's status may still come; Play
  // anyway, for a replay of another version, asks again as `<id>+anyway`). It closing ends it
  // (highlight_playlist.ts), and the banner goes.
  const baseCommand = request?.split("+")[0] ?? null;
  useEffect(() => setStatus(null), [baseCommand]);
  useEffect(
    () =>
      dolphinService.onEvent(DolphinEventType.PLAYBACK_STATUS, ({ status }) => {
        if (baseCommand && status.commandId?.startsWith(baseCommand)) {
          setStatus(status);
        }
      }),
    [dolphinService, baseCommand],
  );

  const play = useCallback(
    async (index: number) => {
      const entry = playlist[index];
      if (!entry) {
        return;
      }
      setSelected(index);
      setDismissed(null);
      await playEntry(sourceId, playlist, entry);
    },
    [playlist, playEntry, sourceId],
  );

  // A click selects the row alone (a double click, its Play button, ↑/↓ and Enter play); Ctrl+click adds
  // or removes it, Shift+click selects the rows from the last one clicked to it (with Ctrl: as well as
  // those selected).
  const selectOnly = (index: number) => {
    const key = entries[index]?.key;
    if (key != null) {
      setTicked(new Set([key]));
      setAnchor(key);
    }
  };
  const open = (index: number) => {
    selectOnly(index);
    void play(index);
  };
  const clickRow = (index: number, e: React.MouseEvent) => {
    const key = entries[index].key;
    const ctrl = e.ctrlKey || e.metaKey;
    if (e.shiftKey) {
      const from = anchor != null ? entries.findIndex((x) => x.key === anchor) : -1;
      const [lo, hi] = from < 0 ? [index, index] : [Math.min(from, index), Math.max(from, index)];
      const range = entries.slice(lo, hi + 1).map((x) => x.key);
      setTicked((t) => new Set(ctrl ? [...t, ...range] : range));
      if (from < 0) {
        setAnchor(key);
      }
    } else if (ctrl) {
      setTicked((t) => {
        const next = new Set(t);
        if (!next.delete(key)) {
          next.add(key);
        }
        return next;
      });
      setAnchor(key);
    } else {
      selectOnly(index);
    }
  };

  // (Only rows still listed count.)
  const chosen = useMemo(() => entries.filter((e) => ticked.has(e.key)), [entries, ticked]);

  // The ticked rows, in the list's order, to video: a file each, or one combo movie.
  const render = useCallback(
    async (music: boolean) => {
      if (!chosen.length) {
        return;
      }
      const asCombo = combo && chosen.length > 1;
      setQueuing(true);
      try {
        const queued = await renderHighlights({
          items: chosen.map((e) => ({
            path: e.path,
            startFrame: e.row.startFrame,
            endFrame: e.row.endFrame,
          })),
          names: asCombo
            ? [comboName(chosen)]
            : chosen.map((e) => clipVideoName(e.row, new Map(e.game.players.map((p) => [p.port, p])), e.path)),
          music,
          combo: asCombo,
        });
        if (queued) {
          setTicked(new Set());
        }
      } finally {
        setQueuing(false);
      }
    },
    [chosen, combo, comboName, renderHighlights],
  );

  // The row playing follows Dolphin's buttons and stars, and is shown only while Dolphin is open (the
  // user, 2026-10-24); ↑/↓ and Enter go from it, or with nothing of this list playing, from the one
  // last played.
  const playingIndex =
    mine && current
      ? entries.findIndex(
          (e) =>
            e.path === current.path && e.row.startFrame === current.startFrame && e.row.endFrame === current.endFrame,
        )
      : -1;
  const playing = playingIndex >= 0 ? playingIndex : null;
  const cursor = mine && current ? playing : selected;
  const keys = useMemo(() => entries.map((e) => e.key), [entries]);
  const shown = useWindowedRows(keys, listRef);
  const { scrollToRow } = shown;
  // (Below the top bar and above the bottom one, both staying as the rows scroll under them.)
  useEffect(() => {
    if (cursor != null) {
      scrollToRow(cursor, headerRef.current?.offsetHeight ?? 0, footerRef.current?.offsetHeight ?? 0);
    }
  }, [cursor, scrollToRow]);

  const step = (delta: number) => {
    if (!entries.length) {
      return;
    }
    const at = cursor ?? (delta > 0 ? -1 : entries.length);
    const next = Math.min(Math.max(at + delta, 0), entries.length - 1);
    if (next !== at) {
      open(next);
    }
  };
  useMousetrap(["down", "tab"], (e) => {
    e.preventDefault();
    step(1);
  });
  useMousetrap(["up", "shift+tab"], (e) => {
    e.preventDefault();
    step(-1);
  });
  useMousetrap("enter", (e) => {
    e.preventDefault();
    open(cursor ?? 0);
  });
  useMousetrap("mod+a", (e) => {
    e.preventDefault();
    setTicked(new Set(entries.map((x) => x.key)));
  });

  const canRender = chosen.length > 0 && !queuing;
  const mixedKinds = useMemo(() => new Set(entries.map((e) => e.row.kind)).size > 1, [entries]);

  return (
    <div className={styles.container}>
      {/* Finding rows on top (one line, staying as the rows scroll under it); what to do with the ones selected at
          the bottom (Sarah's board: the two on two lines of one bar, one to each side, looked off). */}
      {headerExtra && (
        <div className={styles.header} ref={headerRef}>
          {headerExtra}
        </div>
      )}
      {versionNote}
      <RenderJobs jobs={jobs} />
      <PlaybackBanner
        request={request}
        status={status}
        row={playing != null ? entries[playing]?.row : undefined}
        onDismiss={() => setDismissed(commandId)}
      />
      {emptyNote && <div className={styles.note}>{emptyNote}</div>}
      {!hideRows && (
        <div className={styles.list} ref={listRef}>
          <div style={{ height: shown.before }} aria-hidden={true} />
          {entries.slice(shown.start, shown.end).map((entry, j) => {
            const i = shown.start + j;
            const { row, game } = entry;
            const players = new Map(game.players.map((p) => [p.port, p]));
            const stocks = stocksAt(game.stocks, row.startFrame);
            const favorite = favorites.has(favoriteKey(entry));
            // (A heading only where the list has more than one kind: Manual Clips alone needs none.)
            const heading = mixedKinds && row.kind !== "highlight" && (i === 0 || entries[i - 1].row.kind !== row.kind);
            const from = convertFrameCountToDurationString(row.startFrame);
            const to = convertFrameCountToDurationString(row.endFrame);
            return (
              <div key={entry.key} ref={shown.measure(entry.key)} className={styles.item}>
                {heading && (
                  <h3 className={styles.section}>{row.kind === "clip" ? Messages.clips() : Messages.manualClips()}</h3>
                )}
                <div
                  className={`${styles.rowBox} ${i === playing ? styles.playing : ""} ${
                    ticked.has(entry.key) ? styles.selected : ""
                  } ${row.kind === "manual" ? styles.removable : ""}`}
                >
                  {/* The stage (rows of many games), the players, then when in the game it plays and the game's
                      stage and date (Sarah's board, 2026-11-15: a design pass, "i don't like how cluttered it is"). */}
                  <button
                    type="button"
                    data-row={i}
                    className={`${styles.row} ${showDates ? styles.withStage : ""}`}
                    onClick={(e) => clickRow(i, e)}
                    onDoubleClick={() => void play(i)}
                  >
                    {showDates && <StageThumb stageId={game.stageId} />}
                    <span
                      className={`${styles.who} ${styles.rowPlayers} ${
                        (row.kind === "highlight" ? !game.isTeams : isOneOnOne(game)) ? styles.oneLine : ""
                      }`}
                    >
                      {row.kind === "highlight" ? (
                        <HighlightWho row={row} data={game} players={players} stocks={stocks} />
                      ) : (
                        <GameWho data={game} stocks={stocks} />
                      )}
                    </span>
                    <span className={styles.meta}>
                      {showDates && <StageName stageId={game.stageId} />}
                      <span className={styles.time} title={Messages.startsAt(from)}>
                        {from}–{to}
                      </span>
                      {showDates && entry.date && (
                        <span className={styles.date}>{monthDayHourFormat(new Date(entry.date))}</span>
                      )}
                    </span>
                  </button>
                  {/* (Over the row, not in it: a button can't hold another.) A star shows when starred; the rest
                      when the row is pointed at, selected or playing. */}
                  <span className={styles.actions}>
                    <IconButton
                      size="small"
                      className={`${styles.star} ${favorite ? styles.starred : ""}`}
                      onClick={() => toggleFavorite(entry)}
                      aria-label={favorite ? Messages.unfavorite() : Messages.favorite()}
                      aria-pressed={favorite}
                    >
                      {favorite ? <StarIcon fontSize="small" /> : <StarBorderIcon fontSize="small" />}
                    </IconButton>
                    {row.kind === "manual" && (
                      <Tooltip title={Messages.removeClip()} arrow={true}>
                        <IconButton
                          size="small"
                          className={styles.remove}
                          onClick={() => removeManualClip(entry)}
                          aria-label={Messages.removeClip()}
                        >
                          <DeleteOutlineIcon fontSize="small" />
                        </IconButton>
                      </Tooltip>
                    )}
                    <Tooltip title={Messages.play()} arrow={true}>
                      <IconButton
                        size="small"
                        className={styles.play}
                        onClick={() => void play(i)}
                        aria-label={Messages.play()}
                      >
                        <PlayArrowRoundedIcon />
                      </IconButton>
                    </Tooltip>
                  </span>
                </div>
              </div>
            );
          })}
          <div style={{ height: shown.after }} aria-hidden={true} />
        </div>
      )}
      {/* Only with rows selected (a click selects one): rendering is what to do with them. */}
      {chosen.length > 0 && (
        <div className={styles.footer} ref={footerRef}>
          <span className={styles.hint}>{Messages.keyHint()}</span>
          <div className={styles.renderBar}>
            <FormControlLabel
              className={styles.combo}
              control={<Checkbox size="small" checked={combo} onChange={(e) => setCombo(e.target.checked)} />}
              label={<span className={styles.comboLabel}>{Messages.comboMovie()}</span>}
            />
            <Button
              size="small"
              variant="outlined"
              startIcon={<MovieIcon />}
              disabled={!canRender}
              onClick={() => void render(true)}
            >
              {Messages.renderWithMusic()}
            </Button>
            <Button
              size="small"
              variant="outlined"
              startIcon={<MusicOffIcon />}
              disabled={!canRender}
              onClick={() => void render(false)}
            >
              {Messages.renderNoMusic()}
            </Button>
          </div>
        </div>
      )}
    </div>
  );
};

/** The Highlights tab's combo of rows from several games: named after when it was made. */
export const comboOfManyName = (chosen: HighlightEntry[]) => {
  const paths = new Set(chosen.map((e) => e.path));
  if (paths.size === 1) {
    const e = chosen[0];
    return comboVideoName(e.game.players, chosen.length, e.path);
  }
  return multiGameComboName(chosen.length, new Date());
};

/** A player's stocks at a row's start, and at the game's. */
type Stocks = { left: number; start: number };

/** Everyone's stocks at `frame` (none known: an empty map, the chips show none). */
const stocksAt = (changes: StockChange[] | undefined, frame: number): Map<number, Stocks> => {
  const out = new Map<number, Stocks>();
  for (const c of changes ?? []) {
    if (c.frame > frame && out.has(c.port)) {
      continue;
    }
    const known = out.get(c.port);
    out.set(c.port, { left: c.stocks, start: known?.start ?? c.stocks });
  }
  return out;
};

/** Stocks as pips (filled: left, hollow: lost), or "×N" past what pips show. */
const StockPips = ({ stocks }: { stocks: Stocks }) => {
  const label = Messages.stocksLeft(stocks.left, stocks.start);
  if (stocks.start > MAX_PIPS || stocks.left > stocks.start) {
    return (
      <span className={styles.stockCount} title={label}>
        {Messages.stockCount(stocks.left)}
      </span>
    );
  }
  return (
    <span className={styles.pips} title={label} aria-label={label}>
      {Array.from({ length: stocks.start }, (_, i) => (
        <span key={i} className={`${styles.pip} ${i < stocks.left ? styles.pipLeft : ""}`} />
      ))}
    </span>
  );
};

/** A row of many games' lists starts with its game's stage, the stage select's icon. */
const StageThumb = ({ stageId }: { stageId?: number | null }) => {
  const stageIcon = useStageIcon();
  const icon = stageId != null ? stageIcon(stageId) : undefined;
  return icon ? <img className={styles.stageIcon} src={icon} alt="" /> : <span className={styles.stageIcon} />;
};

const StageName = ({ stageId }: { stageId?: number | null }) => {
  const stage = stageId != null ? stageInfo(stageId) : undefined;
  return stage ? <span className={styles.stage}>{stage.name}</span> : null;
};

/** Two players, no teams: X vs Y, which keeps to one line (the names shorten). */
const isOneOnOne = (data: ReplayHighlights) => !data.isTeams && data.players.length === 2;

/** Everyone in the game: one vs another; in teams, team vs team. */
const GameWho = ({ data, stocks }: { data: ReplayHighlights; stocks: Map<number, Stocks> }) => {
  const sides: HighlightPlayer[][] = [];
  if (data.isTeams) {
    const teams = new Map<number | null, HighlightPlayer[]>();
    for (const p of data.players) {
      teams.set(p.teamId, [...(teams.get(p.teamId) ?? []), p]);
    }
    sides.push(...teams.values());
  } else {
    sides.push(...data.players.map((p) => [p]));
  }
  return (
    <>
      {sides.map((side, i) => (
        <React.Fragment key={i}>
          {i > 0 && <span className={styles.vs}>{Messages.vs()}</span>}
          <span className={styles.side}>
            {side.map((p) => (
              <PlayerChip key={p.port} player={p} isTeams={data.isTeams} stocks={stocks.get(p.port)} />
            ))}
          </span>
        </React.Fragment>
      ))}
    </>
  );
};

/** The attacker vs the victim; in teams, the attacker's team vs the victim's. */
const HighlightWho = ({
  row,
  data,
  players,
  stocks,
}: {
  row: ReplayHighlight;
  data: ReplayHighlights;
  players: Map<number, HighlightPlayer>;
  stocks: Map<number, Stocks>;
}) => {
  const side = (port: number) => {
    const p = players.get(port);
    if (!data.isTeams || p?.teamId == null) {
      return [p];
    }
    // The player who did it first, then their teammates.
    return [p, ...data.players.filter((q) => q.teamId === p.teamId && q.port !== port)];
  };
  return (
    <>
      <span className={styles.side}>
        {side(row.attackerPort).map((p, i) => (
          <PlayerChip key={i} player={p} isTeams={data.isTeams} stocks={p && stocks.get(p.port)} />
        ))}
      </span>
      <span className={styles.vs}>{Messages.vs()}</span>
      <span className={styles.side}>
        {side(row.victimPort).map((p, i) => (
          <PlayerChip key={i} player={p} isTeams={data.isTeams} stocks={p && stocks.get(p.port)} />
        ))}
      </span>
    </>
  );
};

const PlayerChip = ({
  player,
  isTeams,
  stocks,
}: {
  player: HighlightPlayer | undefined;
  isTeams: boolean;
  /** At the row's start (a replay from before they were kept: none). */
  stocks?: Stocks;
}) => {
  if (!player) {
    return null;
  }
  const color = getColor(player.port, isTeams ? player.teamId ?? undefined : undefined);
  return (
    <span className={styles.chip} style={{ borderColor: color, "--port-color": color } as React.CSSProperties}>
      <img className={styles.charIcon} src={getCharacterIcon(player.characterId, player.costume)} alt="" />
      <span className={styles.name}>{player.displayName || player.connectCode || `P${player.port}`}</span>
      {stocks && <StockPips stocks={stocks} />}
    </span>
  );
};

/** What the playback Dolphin is doing for the list: starting, loading, fast-forwarding (with its progress), playing. */
const PlaybackBanner = ({
  request,
  status,
  row,
  onDismiss,
}: {
  /** The request's commandId (null: nothing of this list plays, or the banner was closed). */
  request: string | null;
  status: ReplayPlaybackStatus | null;
  row: Row | undefined;
  onDismiss: () => void;
}) => {
  // How long it has been starting (a Dolphin answering a question in its window, a slow PC).
  const [slow, setSlow] = useState(false);
  const starting = !!request && (!status || ["loading", "idle", "waiting"].includes(status.state ?? ""));
  useEffect(() => {
    setSlow(false);
    if (!starting) {
      return;
    }
    const timer = setTimeout(() => setSlow(true), STARTING_HINT_MS);
    return () => clearTimeout(timer);
  }, [starting, request]);
  if (!request) {
    return null;
  }
  const dismiss = (
    <IconButton size="small" className={styles.dismiss} onClick={onDismiss} aria-label={Messages.dismiss()}>
      <CloseIcon fontSize="small" />
    </IconButton>
  );
  const state = status?.state;
  // (A refused replay's error stays in the status after its match has gone.)
  if (state === "error" || (status?.error && state !== "seeking" && state !== "playing")) {
    return (
      <div className={`${styles.banner} ${styles.error}`}>
        <div className={styles.bannerText}>
          {Messages.playbackError(status?.error ?? "")}
          {dismiss}
        </div>
      </div>
    );
  }
  // Its match ended before the clip came (a replay that plays differently here, played anyway).
  if (state === "ended" && status?.frame != null && status.clipStart != null && status.frame < status.clipStart) {
    return (
      <div className={`${styles.banner} ${styles.error}`}>
        <div className={styles.bannerText}>
          {Messages.endedBefore()}
          {dismiss}
        </div>
      </div>
    );
  }
  if (!status || state === "loading" || state === "idle" || state === "waiting") {
    return (
      <div className={styles.banner}>
        <div className={styles.bannerText}>
          {status && state === "waiting" ? Messages.loadingReplay() : Messages.startingDolphin()}
          {dismiss}
        </div>
        {slow && <div className={styles.bannerHint}>{Messages.slowStart()}</div>}
        <LinearProgress className={styles.progress} />
      </div>
    );
  }
  if (state === "seeking") {
    const frame = status.frame ?? 0;
    const target = status.clipStart ?? row?.startFrame ?? 0;
    const from = status.seekFrom != null && status.seekFrom >= 0 ? status.seekFrom : frame;
    const value = target > from ? Math.min(100, Math.max(0, ((frame - from) / (target - from)) * 100)) : 0;
    return (
      <div className={`${styles.banner} ${styles.seeking}`}>
        <div className={styles.bannerText}>
          <span className={styles.ff}>⏩</span>
          {row && row.kind !== "highlight" ? Messages.fastForwardingClip() : Messages.fastForwarding()}
          {/* (Back to an earlier one: a keyframe is put back first, in a moment.) */}
          {frame <= target && <span className={styles.frames}>{Messages.frameOf(Math.max(frame, 0), target)}</span>}
          {dismiss}
        </div>
        <LinearProgress
          key={`seek-${request}-${status.clipStart}`}
          className={styles.progress}
          variant="determinate"
          value={frame <= target ? value : 0}
          color="secondary"
        />
      </div>
    );
  }
  const start = status.clipStart ?? 0;
  const end = status.clipEnd ?? start;
  const frame = status.frame ?? start;
  const value = end > start ? Math.min(100, Math.max(0, ((frame - start) / (end - start)) * 100)) : 100;
  return (
    <div className={styles.banner}>
      <div className={styles.bannerText}>
        {state === "ended" ? Messages.done() : Messages.playing()}
        {dismiss}
      </div>
      <LinearProgress
        key={`play-${request}-${status.clipStart}`}
        className={styles.progress}
        variant="determinate"
        value={state === "ended" ? 100 : value}
      />
    </div>
  );
};
