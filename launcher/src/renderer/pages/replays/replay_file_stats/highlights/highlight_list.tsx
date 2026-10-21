/**
 * A .brep's highlights and L+R+Z clips on its stats page (DESIGN.md §2): who on whom with their
 * characters, best first, nothing the detector measured. A row plays in the playback Dolphin from
 * just before the string to just after it; ↑/↓ (or Tab) plays the next one at once, and the only
 * wait (Dolphin's fast-forward to it) shows with its progress. Ticked rows render to video
 * (docs/design/highlights.md §5): Render (w/ Music) or Render (No Music), a file each, or one
 * combo movie of them all.
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
import { getCharacterIcon } from "@/lib/utils";
import { useServices } from "@/services";

import { HighlightListMessages as Messages } from "./highlight_list.messages";
import styles from "./highlight_list.module.css";
import { RenderJobs, useRenderJobs } from "./render_jobs";
import { clipVideoName, comboVideoName } from "./render_names";
import { ReplayVersionNote } from "./replay_version_note";

type Row = ReplayHighlight | ReplayClip;

/** What the launcher asked the playback Dolphin for, and when (before Dolphin says anything about it). */
type Request = { from: number; count: number; commandId: string };

export const HighlightList = ({ filePath, pplusRelease }: { filePath: string; pplusRelease?: string }) => {
  const { replayService, dolphinService } = useServices();
  const { playHighlights, renderHighlights } = useDolphinActions(dolphinService);
  const [detectionOff] = useSetting("disableHighlightDetection");
  const [clipOnLrz] = useSetting("clipOnLrz");
  const query = useQuery({
    queryKey: ["replayHighlights", filePath, detectionOff, clipOnLrz],
    queryFn: () => replayService.getHighlights(filePath),
  });
  const data = query.data;
  const rows: Row[] = useMemo(() => (data ? [...data.highlights, ...data.clips] : []), [data]);

  const [selected, setSelected] = useState<number | null>(null);
  const [request, setRequest] = useState<Request | null>(null);
  const [status, setStatus] = useState<ReplayPlaybackStatus | null>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const [ticked, setTicked] = useState<Set<number>>(new Set());
  const [combo, setCombo] = useState(false);
  const [queuing, setQueuing] = useState(false);
  const allJobs = useRenderJobs();
  // (A cancelled one is gone: the player did it.)
  const jobs = useMemo(
    () => allJobs.filter((j) => j.paths.includes(filePath) && j.state !== "cancelled"),
    [allJobs, filePath],
  );

  // A new replay (the stats page's next/previous): nothing of the last one's.
  useEffect(() => {
    setSelected(null);
    setRequest(null);
    setStatus(null);
    setTicked(new Set());
    setCombo(false);
  }, [filePath]);

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
      const items = rows.slice(from, from + count).map((r) => ({
        path: filePath,
        startFrame: r.startFrame,
        endFrame: r.endFrame,
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
    [rows, filePath, playHighlights],
  );

  const tick = (i: number, on: boolean) =>
    setTicked((t) => {
      const next = new Set(t);
      if (on) {
        next.add(i);
      } else {
        next.delete(i);
      }
      return next;
    });

  // The ticked rows, in the list's order, to video: a file each, or one combo movie.
  const render = useCallback(
    async (music: boolean) => {
      if (!data || !ticked.size) {
        return;
      }
      const chosen = rows.filter((_, i) => ticked.has(i));
      const players = new Map(data.players.map((p) => [p.port, p]));
      const asCombo = combo && chosen.length > 1;
      setQueuing(true);
      try {
        const queued = await renderHighlights({
          items: chosen.map((r) => ({ path: filePath, startFrame: r.startFrame, endFrame: r.endFrame })),
          names: asCombo
            ? [comboVideoName(data.players, chosen.length, filePath)]
            : chosen.map((r) => clipVideoName(r, players, filePath)),
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
    [data, rows, ticked, combo, filePath, renderHighlights],
  );

  // Play all: the row playing follows Dolphin's queue.
  const playing = request && status && status.index != null ? request.from + status.index : selected;
  useEffect(() => {
    if (playing != null) {
      listRef.current?.querySelector(`[data-row="${playing}"]`)?.scrollIntoView({ block: "nearest" });
    }
  }, [playing]);

  const step = (delta: number) => {
    if (!rows.length) {
      return;
    }
    const at = playing ?? (delta > 0 ? -1 : rows.length);
    const next = Math.min(Math.max(at + delta, 0), rows.length - 1);
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

  if (query.isLoading) {
    return <div className={styles.note}>{Messages.finding()}</div>;
  }
  if (query.error) {
    return <div className={styles.note}>{Messages.couldNotLoad(String(query.error))}</div>;
  }
  if (!data) {
    return null;
  }
  const players = new Map(data.players.map((p) => [p.port, p]));
  const highlightCount = data.highlights.length;

  return (
    <div className={styles.container}>
      <div className={styles.header}>
        <h2 className={styles.title}>{Messages.highlights()}</h2>
        <Button
          variant="contained"
          color="primary"
          size="small"
          startIcon={<PlaylistPlayIcon />}
          disabled={!rows.length}
          onClick={() => void play(0, rows.length)}
        >
          {Messages.playAll()}
        </Button>
      </div>
      <ReplayVersionNote release={pplusRelease} />
      {rows.length > 0 && (
        <div className={styles.renderBar}>
          <Checkbox
            size="small"
            className={styles.tick}
            checked={ticked.size === rows.length}
            indeterminate={ticked.size > 0 && ticked.size < rows.length}
            onChange={(e) => setTicked(e.target.checked ? new Set(rows.map((_, i) => i)) : new Set())}
            inputProps={{ "aria-label": Messages.selectAll() }}
          />
          <span className={styles.renderCount}>
            {ticked.size ? Messages.selected(ticked.size) : Messages.tickToRender()}
          </span>
          <FormControlLabel
            className={styles.combo}
            disabled={ticked.size < 2}
            control={
              <Checkbox size="small" checked={combo && ticked.size > 1} onChange={(e) => setCombo(e.target.checked)} />
            }
            label={<span className={styles.comboLabel}>{Messages.comboMovie()}</span>}
          />
          <Button
            size="small"
            variant="outlined"
            startIcon={<MovieIcon />}
            disabled={!ticked.size || queuing}
            onClick={() => void render(true)}
          >
            {Messages.renderWithMusic()}
          </Button>
          <Button
            size="small"
            variant="outlined"
            startIcon={<MusicOffIcon />}
            disabled={!ticked.size || queuing}
            onClick={() => void render(false)}
          >
            {Messages.renderNoMusic()}
          </Button>
        </div>
      )}
      <RenderJobs jobs={jobs} />
      <PlaybackBanner request={request} status={status} rows={rows} />
      {data.error ? (
        <div className={styles.note}>{Messages.noData()}</div>
      ) : (
        <div className={styles.list} ref={listRef}>
          {detectionOff ? (
            <div className={styles.note}>{Messages.detectionOff()}</div>
          ) : highlightCount === 0 ? (
            <div className={styles.note}>{Messages.noHighlights()}</div>
          ) : null}
          {rows.map((row, i) => (
            <React.Fragment key={i}>
              {i === highlightCount && <h3 className={styles.section}>{Messages.clips()}</h3>}
              <div className={styles.rowLine}>
                <Checkbox
                  size="small"
                  className={styles.tick}
                  checked={ticked.has(i)}
                  onChange={(e) => tick(i, e.target.checked)}
                  inputProps={{ "aria-label": Messages.tickRow() }}
                />
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
                    <HighlightWho row={row} data={data} players={players} />
                  ) : (
                    <span className={styles.who}>
                      <ContentCutIcon fontSize="small" className={styles.clipIcon} />
                      <span className={styles.vs}>{Messages.clippedBy()}</span>
                      <PlayerChip player={players.get(row.port)} isTeams={data.isTeams} />
                    </span>
                  )}
                </button>
              </div>
            </React.Fragment>
          ))}
        </div>
      )}
      {rows.length > 0 && <div className={styles.hint}>{Messages.keyHint()}</div>}
    </div>
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
  rows,
}: {
  request: Request | null;
  status: ReplayPlaybackStatus | null;
  rows: Row[];
}) => {
  if (!request) {
    return null;
  }
  const index = status?.index ?? 0;
  const row = rows[request.from + index];
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
