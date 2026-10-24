/**
 * Highlights and L+R+Z clips (DESIGN.md §2): who on whom with their characters, nothing the
 * detector measured. A row plays in the playback Dolphin from just before the string to just after
 * it; ↑/↓ (or Tab) plays the next one at once, and the only wait (Dolphin's fast-forward to it)
 * shows with its progress. The list played from is the playback Dolphin's "<< < > >>"
 * (highlight_playlist.ts), and the row playing follows them. A click also selects the row (Ctrl+click
 * adds or removes one, Shift+click a range); the selected rows render to video (docs/design/highlights.md
 * §4): Render (w/ Music) or Render (No Music), a file each, or one combo movie of them all.
 * `HighlightList` is a replay's (its stats page); `HighlightRows` lists rows of any replays (the
 * Replays > Highlights tab's, all of them). The star at a row's start makes it a favorite
 * (favorites.ts), the Play button at its end plays it. Manual clips (the seek bar's, manual_clips.ts)
 * come last, each with a button that removes it.
 */
import type { ReplayPlaybackStatus } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import CloseIcon from "@mui/icons-material/Close";
import ContentCutIcon from "@mui/icons-material/ContentCut";
import CropIcon from "@mui/icons-material/Crop";
import DeleteOutlineIcon from "@mui/icons-material/DeleteOutline";
import MovieIcon from "@mui/icons-material/Movie";
import MusicOffIcon from "@mui/icons-material/MusicOff";
import PlayCircleOutlineIcon from "@mui/icons-material/PlayCircleOutline";
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
} from "@replays/highlights/types";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useMousetrap } from "@/lib/hooks/use_mousetrap";
import { useSetting } from "@/lib/hooks/use_settings";
import { getColor } from "@/lib/player_colors";
import { monthDayHourFormat } from "@/lib/time";
import { getCharacterIcon } from "@/lib/utils";
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

type Row = ReplayHighlight | ReplayClip | ReplayManualClip;

/** Starting for this long: the banner says where to look. */
const STARTING_HINT_MS = 20_000;

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
  const query = useQuery({
    queryKey: ["replayHighlights", filePath, detectionOff, clipOnLrz],
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
              .map(manualClipEntry),
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
  /** On the list's top bar, left of the render buttons (the Highlights tab's search and sort). */
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

  // A click selects the row alone and plays it (as ↑/↓ do); Ctrl+click adds or removes it, Shift+click
  // selects the rows from the last one clicked to it (with Ctrl: as well as those selected), neither playing.
  const open = (index: number) => {
    const key = entries[index]?.key;
    if (key != null) {
      setTicked(new Set([key]));
      setAnchor(key);
    }
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
      open(index);
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

  // The row playing follows Dolphin's buttons and stars; with nothing of this list playing, the one
  // last clicked.
  const playingIndex =
    mine && current
      ? entries.findIndex(
          (e) =>
            e.path === current.path && e.row.startFrame === current.startFrame && e.row.endFrame === current.endFrame,
        )
      : -1;
  const playing = mine && current ? (playingIndex >= 0 ? playingIndex : null) : selected;
  useEffect(() => {
    if (playing != null) {
      listRef.current?.querySelector(`[data-row="${playing}"]`)?.scrollIntoView({ block: "nearest" });
    }
  }, [playing]);

  const step = (delta: number) => {
    if (!entries.length) {
      return;
    }
    const at = playing ?? (delta > 0 ? -1 : entries.length);
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
    open(playing ?? 0);
  });
  useMousetrap("mod+a", (e) => {
    e.preventDefault();
    setTicked(new Set(entries.map((x) => x.key)));
  });

  const canRender = chosen.length > 0 && !queuing;

  return (
    <div className={styles.container}>
      {/* One line, staying as the rows scroll under it. */}
      <div className={styles.header}>
        <div className={styles.headerActions}>{headerExtra}</div>
        {entries.length > 0 && (
          <div className={styles.renderBar}>
            <FormControlLabel
              className={styles.combo}
              disabled={chosen.length === 0}
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
        )}
      </div>
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
          {entries.map((entry, i) => {
            const { row, game } = entry;
            const players = new Map(game.players.map((p) => [p.port, p]));
            const favorite = favorites.has(favoriteKey(entry));
            const heading = row.kind !== "highlight" && (i === 0 || entries[i - 1].row.kind !== row.kind);
            return (
              <React.Fragment key={entry.key}>
                {heading && (
                  <h3 className={styles.section}>{row.kind === "clip" ? Messages.clips() : Messages.manualClips()}</h3>
                )}
                <div className={styles.rowBox}>
                  <button
                    type="button"
                    data-row={i}
                    className={`${styles.row} ${i === playing ? styles.playing : ""} ${
                      ticked.has(entry.key) ? styles.selected : ""
                    } ${row.kind === "manual" ? styles.removable : ""}`}
                    onClick={(e) => clickRow(i, e)}
                  >
                    {row.kind === "highlight" ? (
                      <HighlightWho row={row} data={game} players={players} />
                    ) : row.kind === "manual" ? (
                      <span className={styles.who}>
                        <CropIcon fontSize="small" className={styles.clipIcon} />
                        <span className={styles.vs}>{Messages.manualClip(clipLength(row))}</span>
                        <GameWho data={game} />
                      </span>
                    ) : (
                      <span className={styles.who}>
                        <ContentCutIcon fontSize="small" className={styles.clipIcon} />
                        <GameWho data={game} />
                      </span>
                    )}
                    {showDates && entry.date && (
                      <span className={styles.date}>{monthDayHourFormat(new Date(entry.date))}</span>
                    )}
                  </button>
                  {/* (Over the row, not in it: a button can't hold another.) */}
                  <IconButton
                    size="small"
                    className={`${styles.star} ${favorite ? styles.starred : ""}`}
                    onClick={() => toggleFavorite(entry)}
                    aria-label={favorite ? Messages.unfavorite() : Messages.favorite()}
                    aria-pressed={favorite}
                  >
                    {favorite ? <StarIcon fontSize="small" /> : <StarBorderIcon fontSize="small" />}
                  </IconButton>
                  <Tooltip title={Messages.play()} arrow={true}>
                    <IconButton
                      size="small"
                      className={styles.play}
                      onClick={() => void play(i)}
                      aria-label={Messages.play()}
                    >
                      <PlayCircleOutlineIcon />
                    </IconButton>
                  </Tooltip>
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
                </div>
              </React.Fragment>
            );
          })}
        </div>
      )}
      {entries.length > 0 && <div className={styles.hint}>{Messages.keyHint()}</div>}
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

/** A manual clip's length, "0:20". */
const clipLength = (row: ReplayManualClip) => {
  const seconds = Math.max(0, Math.round((row.endFrame - row.startFrame) / 60));
  return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
};

/** Everyone in the game: one vs another; in teams, team vs team. */
const GameWho = ({ data }: { data: ReplayHighlights }) => {
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
              <PlayerChip key={p.port} player={p} isTeams={data.isTeams} />
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
}: {
  row: ReplayHighlight;
  data: ReplayHighlights;
  players: Map<number, HighlightPlayer>;
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
    <span className={styles.who}>
      <span className={styles.side}>
        {side(row.attackerPort).map((p, i) => (
          <PlayerChip key={i} player={p} isTeams={data.isTeams} />
        ))}
      </span>
      <span className={styles.vs}>{Messages.vs()}</span>
      <span className={styles.side}>
        {side(row.victimPort).map((p, i) => (
          <PlayerChip key={i} player={p} isTeams={data.isTeams} />
        ))}
      </span>
    </span>
  );
};

const PlayerChip = ({ player, isTeams }: { player: HighlightPlayer | undefined; isTeams: boolean }) => {
  if (!player) {
    return null;
  }
  const color = getColor(player.port, isTeams ? player.teamId ?? undefined : undefined);
  return (
    <span className={styles.chip} style={{ borderColor: color }}>
      <img className={styles.charIcon} src={getCharacterIcon(player.characterId, player.costume)} alt="" />
      <span className={styles.name}>{player.displayName || player.connectCode || `P${player.port}`}</span>
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
