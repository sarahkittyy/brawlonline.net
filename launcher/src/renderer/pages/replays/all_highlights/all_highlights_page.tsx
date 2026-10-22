/**
 * Replays > Highlights: every replay's highlights and L+R+Z clips in one list (all_highlights.ts
 * finds them: the ones already known at once, the others as they are found, one replay at a time),
 * playable and renderable as on a replay's stats page. A sidebar as Replays' folders: All
 * highlights, or Favorites (the starred ones, favorites.ts), the same list of fewer rows.
 */
import StarIcon from "@mui/icons-material/Star";
import VideoLibraryIcon from "@mui/icons-material/VideoLibrary";
import LinearProgress from "@mui/material/LinearProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import type { GameHighlights } from "@replays/highlights/types";
import React, { useEffect, useMemo, useState } from "react";

import { DualPane } from "@/components/dual_pane";
import { useSetting } from "@/lib/hooks/use_settings";
import { favoriteKey, useFavoriteHighlights } from "@/pages/replays/replay_file_stats/highlights/favorites";
import type { HighlightEntry } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { comboOfManyName, HighlightRows } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { useServices } from "@/services";

import { AllHighlightsMessages as Messages } from "./all_highlights_page.messages";
import styles from "./all_highlights_page.module.css";

type Sort = "best" | "newest";
type View = "all" | "favorites";

export const AllHighlightsPage = React.memo(() => {
  const { replayService } = useServices();
  const [detectionOff] = useSetting("disableHighlightDetection");
  const [clipOnLrz] = useSetting("clipOnLrz");
  const [games, setGames] = useState<Map<string, GameHighlights>>(new Map());
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [sort, setSort] = useState<Sort>("best");
  const [view, setView] = useState<View>("all");
  const { favorites } = useFavoriteHighlights();
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

  const { entries, highlightCount } = useMemo(() => {
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
    if (view === "favorites") {
      const listed = (e: HighlightEntry) => favorites.has(favoriteKey(e)) || shown.has(favoriteKey(e));
      highlights = highlights.filter(listed);
      clips = clips.filter(listed);
    }
    const newest = (a: HighlightEntry, b: HighlightEntry) =>
      (b.date ?? "").localeCompare(a.date ?? "") || a.row.startFrame - b.row.startFrame;
    const score = (e: HighlightEntry) => (e.row.kind === "highlight" ? e.row.score : 0);
    highlights.sort(sort === "best" ? (a, b) => score(b) - score(a) || newest(a, b) : newest);
    clips.sort(newest);
    return { entries: [...highlights, ...clips], highlightCount: highlights.length };
  }, [games, sort, view, favorites, shown]);

  const finding = progress != null && progress.done < progress.total;
  const emptyNote =
    error != null
      ? Messages.couldNotLoad(error)
      : detectionOff && !clipOnLrz
      ? Messages.detectionOff()
      : !loading && !finding && entries.length === 0
      ? view === "favorites"
        ? Messages.noFavorites()
        : Messages.none()
      : null;

  const sidebarItem = (id: View, icon: React.ReactNode, label: string) => (
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
            {sidebarItem("favorites", <StarIcon fontSize="small" />, Messages.favorites())}
            <div className={styles.sideRest} />
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
              entries={entries}
              highlightCount={highlightCount}
              resetKey={view}
              emptyNote={emptyNote}
              showDates={true}
              comboName={comboOfManyName}
              headerExtra={
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
              }
            />
          </div>
        }
      />
    </div>
  );
});
