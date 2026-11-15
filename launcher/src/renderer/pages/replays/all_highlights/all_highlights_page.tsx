/**
 * Replays > Highlights: every replay's highlights and L+R+Z clips in one list (all_highlights.ts
 * finds them: the ones already known at once, the others as they are found, one replay at a time),
 * playable and renderable as on a replay's stats page. A select at the start of the toolbar: All
 * highlights, My Highlights (this machine's player's: the replay's recorder), Opponent Highlights
 * (anyone's not on my side), L+R+Z Highlights (the clips), Manual Clips (the playback Dolphin's
 * seek bar clips, manual_clips.ts), or Favorites (the starred ones, favorites.ts), the same list of
 * fewer rows. The player search above the list (Match history's, player_search.tsx) keeps the rows of every player
 * tagged, and of a player whose name or code has what is typed.
 * Newest first unless Best first is picked.
 */
import { currentClipButtons, hotkeyLabel } from "@common/clip_hotkey";
import ContentCutIcon from "@mui/icons-material/ContentCut";
import CropIcon from "@mui/icons-material/Crop";
import GroupIcon from "@mui/icons-material/Group";
import PersonIcon from "@mui/icons-material/Person";
import StarIcon from "@mui/icons-material/Star";
import VideoLibraryIcon from "@mui/icons-material/VideoLibrary";
import LinearProgress from "@mui/material/LinearProgress";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import type { GameHighlights, HighlightPlayer, ReplayHighlights } from "@replays/highlights/types";
import { CLIP_BEFORE_SECONDS } from "@replays/highlights/types";
import type { ReplayPlayer } from "@replays/types";
import React, { useEffect, useMemo, useState } from "react";

import { ReplayPlayerSearch } from "@/components/player_search/replay_player_search";
import { useSetting } from "@/lib/hooks/use_settings";
import { favoriteKey, useFavoriteHighlights } from "@/pages/replays/replay_file_stats/highlights/favorites";
import type { HighlightEntry } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { comboOfManyName, HighlightRows } from "@/pages/replays/replay_file_stats/highlights/highlight_list";
import { manualClipEntry, useManualClips } from "@/pages/replays/replay_file_stats/highlights/manual_clips";
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

type Who = (p: HighlightPlayer) => boolean;

/** Has `query` (lower case) in their name or code. */
const named =
  (query: string): Who =>
  (p) =>
    p.displayName.toLowerCase().includes(query) || p.connectCode.toLowerCase().includes(query);

/** A player tagged in the search (by their code). */
const coded =
  (code: string): Who =>
  (p) =>
    p.connectCode.toLowerCase() === code;

/** A player of the game is `who`. */
const hasPlayer = (game: ReplayHighlights, who: Who) => game.players.some(who);

/**
 * A player the row shows is `who`: a highlight's attacker and victim (in teams, with their
 * teammates), an L+R+Z clip's player; not the others in the game (a free-for-all's third player).
 * (A manual clip's row shows every player: hasPlayer.)
 */
const rowHasPlayer = (e: HighlightEntry, who: Who) => {
  const { row, game } = e;
  if (row.kind === "manual") {
    return hasPlayer(game, who);
  }
  const ports = row.kind === "highlight" ? [row.attackerPort, row.victimPort] : [row.port];
  return game.players.some((p) => ports.some((port) => sameSide(game, port, p.port)) && who(p));
};

export const AllHighlightsPage = React.memo(() => {
  const { replayService } = useServices();
  const [detectionOff] = useSetting("disableHighlightDetection");
  const [clipOnLrz] = useSetting("clipOnLrz");
  const [clipHotkeys] = useSetting("clipHotkeys");
  const hotkey = hotkeyLabel(currentClipButtons(clipHotkeys));
  const [games, setGames] = useState<Map<string, GameHighlights>>(new Map());
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [sort, setSort] = useState<Sort>("newest");
  const [view, setView] = useState<View>("all");
  const [search, setSearch] = useState("");
  const [tagged, setTagged] = useState<ReplayPlayer[]>([]);
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
  }, [replayService, detectionOff, clipOnLrz, clipHotkeys]);

  const query = search.trim().toLowerCase();
  // Every player tagged, and one with what is typed.
  const whos = useMemo(
    () => [...tagged.map((p) => coded(p.connectCode.toLowerCase())), ...(query ? [named(query)] : [])],
    [tagged, query],
  );
  const searching = whos.length > 0;
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
      .map((c) => manualClipEntry(c, games.get(c.path)?.highlights))
      .filter((e) => whos.every((who) => hasPlayer(e.game, who)));
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
    if (searching) {
      highlights = highlights.filter((e) => whos.every((who) => rowHasPlayer(e, who)));
      clips = clips.filter((e) => whos.every((who) => rowHasPlayer(e, who)));
    }
    highlights.sort(sort === "best" ? (a, b) => score(b) - score(a) || newest(a, b) : newest);
    clips.sort(newest);
    return [...highlights, ...clips, ...manual];
  }, [games, sort, view, favorites, shown, whos, searching, manualClips]);

  const finding = progress != null && progress.done < progress.total;
  const searchWords = [...tagged.map((p) => p.connectCode), search.trim()].filter(Boolean).join(", ");
  const emptyNote =
    view === "manual"
      ? entries.length === 0
        ? searching
          ? Messages.noMatch(searchWords)
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
      ? searching
        ? Messages.noMatch(searchWords)
        : view === "favorites"
        ? Messages.noFavorites()
        : view === "mine"
        ? Messages.noMine()
        : view === "opponents"
        ? Messages.noOpponents()
        : view === "clips"
        ? Messages.noClips(hotkey, CLIP_BEFORE_SECONDS)
        : Messages.none()
      : null;

  // The categories as a select at the start of the toolbar (Sarah's board, 2026-11-15: with the app's sidebar and the
  // players' rail, a column of them left the list too little room), each with its icon and, for the clips, how to make
  // them.
  const views: { id: View; icon: React.ReactNode; label: string; hint?: string }[] = [
    { id: "all", icon: <VideoLibraryIcon fontSize="small" />, label: Messages.allHighlights() },
    { id: "mine", icon: <PersonIcon fontSize="small" />, label: Messages.myHighlights() },
    { id: "opponents", icon: <GroupIcon fontSize="small" />, label: Messages.opponentHighlights() },
    {
      id: "clips",
      icon: <ContentCutIcon fontSize="small" />,
      label: Messages.lrzHighlights(),
      hint: Messages.lrzHint(hotkey, CLIP_BEFORE_SECONDS),
    },
    { id: "manual", icon: <CropIcon fontSize="small" />, label: Messages.manualClips(), hint: Messages.manualHint() },
    { id: "favorites", icon: <StarIcon fontSize="small" />, label: Messages.favorites() },
  ];
  const viewSelect = (
    <Select
      size="small"
      value={view}
      onChange={(e) => openView(e.target.value as View)}
      className={styles.viewSelect}
      data-testid="highlights-view"
      renderValue={(id) => {
        const v = views.find((x) => x.id === id);
        return (
          <span className={styles.viewValue}>
            {v?.icon}
            {v?.label}
          </span>
        );
      }}
      MenuProps={{
        // (Under the select, its left edge on the select's: not over the app's sidebar.)
        anchorOrigin: { vertical: "bottom", horizontal: "left" },
        transformOrigin: { vertical: "top", horizontal: "left" },
        slotProps: { paper: { className: styles.viewMenu } },
      }}
    >
      {views.map((v) => (
        <MenuItem key={v.id} value={v.id} className={styles.viewItem}>
          <span className={styles.viewIcon}>{v.icon}</span>
          <span>
            <span className={styles.viewLabel}>{v.label}</span>
            {v.hint && <span className={styles.viewHint}>{v.hint}</span>}
          </span>
        </MenuItem>
      ))}
    </Select>
  );

  return (
    <div className={styles.outer}>
      <div className={styles.split}>
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
                {viewSelect}
                <ReplayPlayerSearch
                  value={tagged}
                  onChange={setTagged}
                  text={search}
                  onTextChange={setSearch}
                  placeholder={Messages.search()}
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
      </div>
    </div>
  );
});
