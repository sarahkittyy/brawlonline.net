/**
 * Replays > Highlights: every replay's highlights and L+R+Z clips in one list (all_highlights.ts
 * finds them: the ones already known at once, the others as they are found, one replay at a time),
 * playable and renderable as on a replay's stats page. A sidebar as Replays' folders: All
 * highlights, My Highlights (this machine's player's: the replay's recorder), Opponent Highlights
 * (anyone's not on my side), L+R+Z Highlights (the clips), Manual Clips (the playback Dolphin's
 * seek bar clips, manual_clips.ts), or Favorites (the starred ones, favorites.ts), the same list of
 * fewer rows. The search above the list keeps the games with a player whose name or code has it.
 * Newest first unless Best first is picked. Below the sidebar, buttons open the Root Replay Folder
 * and the rendered videos' folder (Settings > Clips).
 */
import CloseIcon from "@mui/icons-material/Close";
import ContentCutIcon from "@mui/icons-material/ContentCut";
import CropIcon from "@mui/icons-material/Crop";
import FolderIcon from "@mui/icons-material/Folder";
import GroupIcon from "@mui/icons-material/Group";
import PersonIcon from "@mui/icons-material/Person";
import SearchIcon from "@mui/icons-material/Search";
import StarIcon from "@mui/icons-material/Star";
import VideoLibraryIcon from "@mui/icons-material/VideoLibrary";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import InputBase from "@mui/material/InputBase";
import LinearProgress from "@mui/material/LinearProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import Tooltip from "@mui/material/Tooltip";
import type { GameHighlights, HighlightPlayer, ReplayHighlights } from "@replays/highlights/types";
import { CLIP_BEFORE_SECONDS } from "@replays/highlights/types";
import React, { useEffect, useMemo, useState } from "react";

import { DualPane } from "@/components/dual_pane";
import { useSetting } from "@/lib/hooks/use_settings";
import { useToasts } from "@/lib/hooks/use_toasts";
import { favoriteKey, useFavoriteHighlights } from "@/pages/replays/replay_file_stats/highlights/favorites";
import type { HighlightEntry } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { comboOfManyName, HighlightRows } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { manualClipEntry, useManualClips } from "@/pages/replays/replay_file_stats/highlights/manual_clips";
import { defaultClipsFolder } from "@/pages/settings/clips_settings/clips_settings";
import { useServices } from "@/services";

import { AllHighlightsMessages as Messages } from "./all_highlights_page.messages";
import styles from "./all_highlights_page.module.css";

type Sort = "best" | "newest";
type View = "all" | "mine" | "opponents" | "clips" | "manual" | "favorites";

/** `a` and `b` are on the same side: the same player, or teammates in a team battle. */
const sameSide = (game: ReplayHighlights, a: number, b: number) => {
  if (a === b) {
    return true;
  }
  if (!game.isTeams) {
    return false;
  }
  const team = (port: number) => game.players.find((p) => p.port === port)?.teamId ?? null;
  return team(a) != null && team(a) === team(b);
};

const named = (p: HighlightPlayer, query: string) =>
  p.displayName.toLowerCase().includes(query) || p.connectCode.toLowerCase().includes(query);

/** A player of the game has `query` (lower case) in their name or code. */
const hasPlayer = (game: ReplayHighlights, query: string) => game.players.some((p) => named(p, query));

/**
 * A player the row shows has `query`: a highlight's attacker and victim (in teams, with their
 * teammates), an L+R+Z clip's player; not the others in the game (a free-for-all's third player).
 * (A manual clip's row shows every player: hasPlayer.)
 */
const rowHasPlayer = (e: HighlightEntry, query: string) => {
  const { row, game } = e;
  if (row.kind === "manual") {
    return hasPlayer(game, query);
  }
  const ports = row.kind === "highlight" ? [row.attackerPort, row.victimPort] : [row.port];
  return game.players.some((p) => ports.some((port) => sameSide(game, port, p.port)) && named(p, query));
};

export const AllHighlightsPage = React.memo(() => {
  const { replayService } = useServices();
  const [detectionOff] = useSetting("disableHighlightDetection");
  const [clipOnLrz] = useSetting("clipOnLrz");
  const [rootSlpPath] = useSetting("rootSlpPath");
  const [clipsFolder] = useSetting("clipsFolder");
  const { showInfo } = useToasts();
  const [games, setGames] = useState<Map<string, GameHighlights>>(new Map());
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [sort, setSort] = useState<Sort>("newest");
  const [view, setView] = useState<View>("all");
  const [search, setSearch] = useState("");
  const { favorites } = useFavoriteHighlights();
  const { clips: manualClips } = useManualClips();
  // Favorites keeps the rows unstarred while it is open (starred again in a click), not moving the others up.
  const [shown, setShown] = useState<ReadonlySet<string>>(new Set());
  const openView = (next: View) => {
    setShown(new Set(favorites));
    setView(next);
  };

  // What is known at once, then the replays not looked at yet as they are found.
  useEffect(() => {
    let live = true;
    setLoading(true);
    setGames(new Map());
    setProgress(null);
    const off = replayService.onAllHighlights((event) => {
      if (!live) {
        return;
      }
      setProgress({ done: event.done, total: event.total });
      if (event.game) {
        const game = event.game;
        setGames((m) => new Map(m).set(game.path, game));
      }
    });
    replayService
      .getAllHighlights()
      .then(({ games: known, pending, total }) => {
        if (!live) {
          return;
        }
        setGames((m) => new Map([...known.map((g) => [g.path, g] as const), ...m]));
        setProgress((p) => p ?? (pending > 0 ? { done: total - pending, total } : null));
      })
      .catch((err) => live && setError(err instanceof Error ? err.message : String(err)))
      .finally(() => live && setLoading(false));
    return () => {
      live = false;
      off();
    };
  }, [replayService, detectionOff, clipOnLrz]);

  const query = search.trim().toLowerCase();
  const entries = useMemo(() => {
    const all = [...games.values()];
    let highlights: HighlightEntry[] = all.flatMap((g) =>
      g.highlights.highlights.map((row, i) => ({
        key: `${g.path}#h${i}`,
        path: g.path,
        game: g.highlights,
        row,
        date: g.date,
      })),
    );
    let clips: HighlightEntry[] = all.flatMap((g) =>
      g.highlights.clips.map((row, i) => ({
        key: `${g.path}#c${i}`,
        path: g.path,
        game: g.highlights,
        row,
        date: g.date,
      })),
    );
    // Newest saved first.
    let manual: HighlightEntry[] = [...manualClips]
      .sort((a, b) => b.savedAt.localeCompare(a.savedAt))
      .map(manualClipEntry)
      .filter((e) => !query || hasPlayer(e.game, query));
    // (A replay that doesn't say which player recorded it is in neither mine nor my opponents'.)
    const attacker = (e: HighlightEntry) => (e.row.kind === "highlight" ? e.row.attackerPort : 0);
    if (view === "mine") {
      highlights = highlights.filter((e) => e.game.recorderPort != null && attacker(e) === e.game.recorderPort);
      clips = [];
      manual = [];
    } else if (view === "opponents") {
      highlights = highlights.filter((e) => {
        const me = e.game.recorderPort;
        return me != null && !sameSide(e.game, attacker(e), me);
      });
      clips = [];
      manual = [];
    } else if (view === "clips") {
      highlights = [];
      manual = [];
    } else if (view === "manual") {
      highlights = [];
      clips = [];
    } else if (view === "favorites") {
      const listed = (e: HighlightEntry) => favorites.has(favoriteKey(e)) || shown.has(favoriteKey(e));
      highlights = highlights.filter(listed);
      clips = clips.filter(listed);
      manual = manual.filter(listed);
    }
    const newest = (a: HighlightEntry, b: HighlightEntry) =>
      (b.date ?? "").localeCompare(a.date ?? "") || a.row.startFrame - b.row.startFrame;
    const score = (e: HighlightEntry) => (e.row.kind === "highlight" ? e.row.score : 0);
    if (query) {
      highlights = highlights.filter((e) => rowHasPlayer(e, query));
      clips = clips.filter((e) => rowHasPlayer(e, query));
    }
    highlights.sort(sort === "best" ? (a, b) => score(b) - score(a) || newest(a, b) : newest);
    clips.sort(newest);
    return [...highlights, ...clips, ...manual];
  }, [games, sort, view, favorites, shown, query, manualClips]);

  const finding = progress != null && progress.done < progress.total;
  const emptyNote =
    view === "manual"
      ? entries.length === 0
        ? query
          ? Messages.noMatch(search.trim())
          : Messages.noManual()
        : null
      : error != null
      ? Messages.couldNotLoad(error)
      : detectionOff && !clipOnLrz && entries.length === 0
      ? Messages.detectionOff()
      : (view === "mine" || view === "opponents") && detectionOff
      ? Messages.highlightsOff()
      : view === "clips" && !clipOnLrz
      ? Messages.clipsOff()
      : !loading && !finding && entries.length === 0
      ? query
        ? Messages.noMatch(search.trim())
        : view === "favorites"
        ? Messages.noFavorites()
        : view === "mine"
        ? Messages.noMine()
        : view === "opponents"
        ? Messages.noOpponents()
        : view === "clips"
        ? Messages.noClips(CLIP_BEFORE_SECONDS)
        : Messages.none()
      : null;

  const sidebarItem = (id: View, icon: React.ReactNode, label: string, hint?: string) => {
    const item = (
      <button
        type="button"
        className={`${styles.sideItem} ${view === id ? styles.sideSelected : ""}`}
        onClick={() => openView(id)}
        aria-current={view === id}
      >
        {icon}
        <span className={styles.sideLabel}>{label}</span>
      </button>
    );
    return hint ? (
      <Tooltip title={hint} placement="right" arrow={true}>
        {item}
      </Tooltip>
    ) : (
      item
    );
  };

  const openFolder = async (folder: string) => {
    // (Electron's error text, or "" when it opened.) The videos' folder is made with the first video.
    if (await window.electron.shell.openPath(folder)) {
      showInfo(Messages.folderNotThereYet(folder));
    }
  };
  const folderButton = (label: string, folder: string) => (
    <Tooltip title={folder} placement="right" arrow={true}>
      <button
        type="button"
        className={`${styles.sideItem} ${styles.folderItem}`}
        onClick={() => void openFolder(folder)}
      >
        <FolderIcon fontSize="small" />
        <span className={styles.sideLabel}>{label}</span>
      </button>
    </Tooltip>
  );

  return (
    <div className={styles.outer}>
      <DualPane
        id="highlights"
        resizable={true}
        minWidth={120}
        maxWidth={300}
        width={200}
        leftSide={
          <div className={styles.sidebar}>
            {sidebarItem("all", <VideoLibraryIcon fontSize="small" />, Messages.allHighlights())}
            {sidebarItem("mine", <PersonIcon fontSize="small" />, Messages.myHighlights())}
            {sidebarItem("opponents", <GroupIcon fontSize="small" />, Messages.opponentHighlights())}
            {sidebarItem(
              "clips",
              <ContentCutIcon fontSize="small" />,
              Messages.lrzHighlights(),
              Messages.lrzHint(CLIP_BEFORE_SECONDS),
            )}
            {sidebarItem("manual", <CropIcon fontSize="small" />, Messages.manualClips(), Messages.manualHint())}
            {sidebarItem("favorites", <StarIcon fontSize="small" />, Messages.favorites())}
            <div className={styles.sideRest} />
            {folderButton(Messages.openReplayLocation(), rootSlpPath)}
            {folderButton(Messages.openVideoLocation(), clipsFolder ?? defaultClipsFolder(rootSlpPath))}
          </div>
        }
        rightSide={
          <div className={styles.content}>
            {(loading || finding) && (
              <div className={styles.finding}>
                <div className={styles.findingText}>
                  {progress && finding ? Messages.findingOf(progress.done, progress.total) : Messages.finding()}
                </div>
                <LinearProgress
                  variant={progress && finding ? "determinate" : "indeterminate"}
                  value={progress && progress.total ? (progress.done / progress.total) * 100 : 0}
                />
              </div>
            )}
            <HighlightRows
              sourceId="highlights"
              entries={entries}
              resetKey={view}
              emptyNote={emptyNote}
              showDates={true}
              comboName={comboOfManyName}
              headerExtra={
                <>
                  <InputBase
                    className={styles.search}
                    placeholder={Messages.search()}
                    value={search}
                    onChange={(e) => setSearch(e.target.value)}
                    inputProps={{ "aria-label": Messages.search() }}
                    endAdornment={
                      <InputAdornment position="end">
                        <IconButton size="small" onClick={() => setSearch("")} disabled={search.length === 0}>
                          {search.length > 0 ? <CloseIcon fontSize="inherit" /> : <SearchIcon fontSize="inherit" />}
                        </IconButton>
                      </InputAdornment>
                    }
                  />
                  <Select
                    size="small"
                    value={sort}
                    onChange={(e) => setSort(e.target.value as Sort)}
                    className={styles.sort}
                    inputProps={{ "aria-label": Messages.sort() }}
                  >
                    <MenuItem value="best">{Messages.bestFirst()}</MenuItem>
                    <MenuItem value="newest">{Messages.newestFirst()}</MenuItem>
                  </Select>
                </>
              }
            />
          </div>
        }
      />
    </div>
  );
});
