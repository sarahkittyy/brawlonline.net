/**
 * Highlights and L+R+Z clips (DESIGN.md §2): who on whom with their characters, nothing the
 * detector measured. A row plays in the playback Dolphin from just before the string to just after
 * it; ↑/↓ (or Tab) plays the next one at once, and the only wait (Dolphin's fast-forward to it)
 * shows with its progress. With "Render as Video" ticked, rows can be ticked and rendered to video
 * (docs/design/highlights.md §4): Render (w/ Music) or Render (No Music), a file each, or one combo
 * movie of them all. `HighlightList` is a replay's (its stats page); `HighlightRows` lists rows of
 * any replays (the Replays > Highlights tab's, all of them).
 */
import type { ReplayPlaybackStatus } from "@dolphin/types";
import { DolphinEventType } from "@dolphin/types";
import ContentCutIcon from "@mui/icons-material/ContentCut";
import MovieIcon from "@mui/icons-material/Movie";
import MusicOffIcon from "@mui/icons-material/MusicOff";
import PlayArrowIcon from "@mui/icons-material/PlayArrow";
import PlaylistPlayIcon from "@mui/icons-material/PlaylistPlay";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import FormControlLabel from "@mui/material/FormControlLabel";
import LinearProgress from "@mui/material/LinearProgress";
import type { HighlightPlayer, ReplayClip, ReplayHighlight, ReplayHighlights } from "@replays/highlights/types";
import { useQuery } from "@tanstack/react-query";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useMousetrap } from "@/lib/hooks/use_mousetrap";
import { useSetting } from "@/lib/hooks/use_settings";
import { getColor } from "@/lib/player_colors";
import { monthDayHourFormat } from "@/lib/time";
import { getCharacterIcon } from "@/lib/utils";
import { useServices } from "@/services";

import { HighlightListMessages as Messages } from "./highlight_list.messages";
import styles from "./highlight_list.module.css";
import { RenderJobs, useRenderJobs } from "./render_jobs";
import { clipVideoName, comboVideoName, multiGameComboName } from "./render_names";
import { ReplayVersionNote } from "./replay_version_note";

type Row = ReplayHighlight | ReplayClip;

/** A row: which replay it is of (its players, when it was played) and what of it. */
export type HighlightEntry = {
  key: string;
  path: string;
  game: ReplayHighlights;
  row: Row;
  /** When the game was played (ISO): shown on the row when the list has several games. */
  date?: string;
};

/** What the launcher asked the playback Dolphin for, and when (before Dolphin says anything about it). */
type Request = { from: number; count: number; commandId: string };

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
  const entries: HighlightEntry[] = useMemo(
    () =>
      data
        ? [...data.highlights, ...data.clips].map((row, i) => ({ key: `${i}`, path: filePath, game: data, row }))
        : [],
    [data, filePath],
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
      entries={entries}
      highlightCount={data.highlights.length}
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
      hideRows={!!data.error}
      comboName={(chosen) => comboVideoName(data.players, chosen.length, filePath)}
    />
  );
};

type HighlightRowsProps = {
  entries: HighlightEntry[];
  /** The first this many are highlights, the rest L+R+Z clips. */
  highlightCount: number;
  /** Changes when the list is another (a new replay): nothing of the last one's is kept. */
  resetKey: string;
  versionNote?: React.ReactNode;
  /** Said above the rows (none found, detection off...). */
  emptyNote?: string | null;
  hideRows?: boolean;
  /** Shows each row's game date (rows of many games). */
  showDates?: boolean;
  /** Above the list, right of the title (the Highlights tab's sort). */
  headerExtra?: React.ReactNode;
  comboName: (chosen: HighlightEntry[]) => string;
};

export const HighlightRows = ({
  entries,
  highlightCount,
  resetKey,
  versionNote,
  emptyNote,
  hideRows,
  showDates,
  headerExtra,
  comboName,
}: HighlightRowsProps) => {
  const { dolphinService } = useServices();
  const { playHighlights, renderHighlights } = useDolphinActions(dolphinService);

  const [selected, setSelected] = useState<number | null>(null);
  const [request, setRequest] = useState<Request | null>(null);
  const [status, setStatus] = useState<ReplayPlaybackStatus | null>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const [renderMode, setRenderMode] = useState(false);
  const [ticked, setTicked] = useState<Set<string>>(new Set());
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
    setRequest(null);
    setStatus(null);
    setTicked(new Set());
    setCombo(false);
  }, [resetKey]);

  // Only what Dolphin says about the last request (an older one's status may still come).
  const commandId = request?.commandId;
  useEffect(
    () =>
      dolphinService.onEvent(DolphinEventType.PLAYBACK_STATUS, ({ status }) => {
        // (Play anyway, for a replay of another version, asks again as `<id>+anyway`.)
        if (commandId && status.commandId?.startsWith(commandId)) {
          setStatus(status);
        }
      }),
    [dolphinService, commandId],
  );

  const play = useCallback(
    async (from: number, count = 1) => {
      const items = entries.slice(from, from + count).map((e) => ({
        path: e.path,
        startFrame: e.row.startFrame,
        endFrame: e.row.endFrame,
      }));
      if (!items.length) {
        return;
      }
      const commandId = `hl-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
      setSelected(from);
      setRequest({ from, count: items.length, commandId });
      setStatus(null);
      if (!(await playHighlights(items, commandId))) {
        setRequest(null);
      }
    },
    [entries, playHighlights],
  );

  const tick = (key: string, on: boolean) =>
    setTicked((t) => {
      const next = new Set(t);
      if (on) {
        next.add(key);
      } else {
        next.delete(key);
      }
      return next;
    });

  const setRender = (on: boolean) => {
    setRenderMode(on);
    if (!on) {
      setTicked(new Set());
      setCombo(false);
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
          items: chosen.map((e) => ({ path: e.path, startFrame: e.row.startFrame, endFrame: e.row.endFrame })),
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

  // Play all: the row playing follows Dolphin's queue.
  const playing = request && status && status.index != null ? request.from + status.index : selected;
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
      void play(next);
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
    void play(playing ?? 0);
  });

  const canRender = renderMode && chosen.length > 0 && !queuing;

  return (
    <div className={styles.container}>
      <div className={styles.header}>
        <h2 className={styles.title}>{Messages.highlights()}</h2>
        <div className={styles.headerActions}>
          {headerExtra}
          <Button
            variant="contained"
            color="primary"
            size="small"
            startIcon={<PlaylistPlayIcon />}
            disabled={!entries.length}
            onClick={() => void play(0, entries.length)}
          >
            {Messages.playAll()}
          </Button>
        </div>
      </div>
      {versionNote}
      {entries.length > 0 && (
        <div className={styles.renderBar}>
          <FormControlLabel
            className={styles.renderToggle}
            control={<Checkbox size="small" checked={renderMode} onChange={(e) => setRender(e.target.checked)} />}
            label={<span className={styles.renderToggleLabel}>{Messages.renderAsVideo()}</span>}
          />
          <span className={styles.renderCount}>
            {renderMode && (
              <>
                {Messages.selected(chosen.length)}
                <Button
                  size="small"
                  className={styles.selectAll}
                  onClick={() =>
                    setTicked(chosen.length === entries.length ? new Set() : new Set(entries.map((e) => e.key)))
                  }
                >
                  {chosen.length === entries.length ? Messages.selectNone() : Messages.selectAll()}
                </Button>
              </>
            )}
          </span>
          <FormControlLabel
            className={styles.combo}
            disabled={!renderMode}
            control={
              <Checkbox size="small" checked={renderMode && combo} onChange={(e) => setCombo(e.target.checked)} />
            }
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
      <RenderJobs jobs={jobs} />
      <PlaybackBanner request={request} status={status} entries={entries} />
      {emptyNote && <div className={styles.note}>{emptyNote}</div>}
      {!hideRows && (
        <div className={styles.list} ref={listRef}>
          {entries.map((entry, i) => {
            const { row, game } = entry;
            const players = new Map(game.players.map((p) => [p.port, p]));
            return (
              <React.Fragment key={entry.key}>
                {i === highlightCount && <h3 className={styles.section}>{Messages.clips()}</h3>}
                <div className={styles.rowLine}>
                  {renderMode && (
                    <Checkbox
                      size="small"
                      className={styles.tick}
                      checked={ticked.has(entry.key)}
                      onChange={(e) => tick(entry.key, e.target.checked)}
                      inputProps={{ "aria-label": Messages.tickRow() }}
                    />
                  )}
                  <button
                    type="button"
                    data-row={i}
                    className={`${styles.row} ${i === playing ? styles.playing : ""}`}
                    onClick={() => void play(i)}
                  >
                    <span className={styles.playIcon}>
                      <PlayArrowIcon fontSize="small" />
                    </span>
                    {row.kind === "highlight" ? (
                      <HighlightWho row={row} data={game} players={players} />
                    ) : (
                      <span className={styles.who}>
                        <ContentCutIcon fontSize="small" className={styles.clipIcon} />
                        <span className={styles.vs}>{Messages.clippedBy()}</span>
                        <PlayerChip player={players.get(row.port)} isTeams={game.isTeams} />
                      </span>
                    )}
                    {showDates && entry.date && (
                      <span className={styles.date}>{monthDayHourFormat(new Date(entry.date))}</span>
                    )}
                  </button>
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
  entries,
}: {
  request: Request | null;
  status: ReplayPlaybackStatus | null;
  entries: HighlightEntry[];
}) => {
  if (!request) {
    return null;
  }
  const index = status?.index ?? 0;
  const row = entries[request.from + index]?.row;
  const of = request.count > 1 ? ` · ${Messages.ofTotal(index + 1, request.count)}` : "";
  const state = status?.state;
  // (A refused replay's error stays in the status after its match has gone.)
  if (state === "error" || (status?.error && state !== "seeking" && state !== "playing")) {
    return <div className={`${styles.banner} ${styles.error}`}>{Messages.playbackError(status?.error ?? "")}</div>;
  }
  // Its match ended before the clip came (a replay that plays differently here, played anyway).
  if (state === "ended" && status?.frame != null && status.clipStart != null && status.frame < status.clipStart) {
    return <div className={`${styles.banner} ${styles.error}`}>{Messages.endedBefore()}</div>;
  }
  if (!status || state === "loading" || state === "idle" || state === "waiting") {
    return (
      <div className={styles.banner}>
        <div className={styles.bannerText}>
          {status && state === "waiting" ? Messages.loadingReplay() : Messages.startingDolphin()}
          {of}
        </div>
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
          {row?.kind === "clip" ? Messages.fastForwardingClip() : Messages.fastForwarding()}
          {of}
          {/* (Back to an earlier one: a keyframe is put back first, in a moment.) */}
          {frame <= target && <span className={styles.frames}>{Messages.frameOf(Math.max(frame, 0), target)}</span>}
        </div>
        <LinearProgress
          key={`seek-${request.commandId}-${index}`}
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
        {of}
      </div>
      <LinearProgress
        key={`play-${request.commandId}-${index}`}
        className={styles.progress}
        variant="determinate"
        value={state === "ended" ? 100 : value}
      />
    </div>
  );
};
