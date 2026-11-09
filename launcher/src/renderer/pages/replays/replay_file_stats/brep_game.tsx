import ErrorIcon from "@mui/icons-material/Error";
import HelpIcon from "@mui/icons-material/Help";
import type { FileResult } from "@replays/types";
import type { StatsType } from "@slippi/slippi-js";
import React, { useCallback, useState } from "react";

import { LoadingScreen } from "@/components/loading_screen/loading_screen";
import { IconMessage } from "@/components/message";
import { Tabs } from "@/pages/home/tabs/tabs";

import styles from "./brep_game.module.css";
import { GameProfile } from "./game_profile";
import { HighlightList } from "./highlights/highlight_list";
import type { PlaylistEntry } from "./highlights/highlight_nav";
import { useHighlightPlayer } from "./highlights/highlight_playlist";
import { ReplayFileStatsMessages as Messages } from "./replay_file_stats.messages";

/** "Play from here" starts this long before the moment (as a highlight does: 1.5 s). */
const LEAD_IN_FRAMES = 90;

type Tab = "stats" | "highlights";

/**
 * Plays a replay from a moment the stats name (a stock's start, a punish's first hit) to its end.
 * Dolphin seeks only in its highlights mode, so it is a one-entry highlight list.
 */
const useStatsPlayer = (lastFrame: number | undefined) => {
  const playEntry = useHighlightPlayer();
  return useCallback(
    ({ path, startFrame }: { path: string; startFrame: number }) => {
      const start = Math.max(0, startFrame - LEAD_IN_FRAMES);
      const entry: PlaylistEntry = {
        key: `stats:${path}:${startFrame}`,
        path,
        startFrame: start,
        endFrame: Math.max(start, lastFrame ?? start),
      };
      void playEntry(`stats:${path}`, [entry], entry);
    },
    [playEntry, lastFrame],
  );
};

type Props = {
  filePath: string;
  file: FileResult;
  stats: StatsType | undefined;
  /** The stats are still being computed. */
  loading: boolean;
  error: string | null;
};

/** One of our games (.brep): its stats (Slippi's page) and its highlights and clips, in two tabs. */
export const BrepGame = ({ filePath, file, stats, loading, error }: Props) => {
  const singles = file.game.players.length === 2;
  // A free-for-all has no stats (as Slippi's): its highlights first.
  const [tab, setTab] = useState<Tab>(singles ? "stats" : "highlights");
  const onPlay = useStatsPlayer(stats?.lastFrame);

  let statsView: React.ReactNode;
  if (!singles) {
    statsView = <IconMessage Icon={ErrorIcon} label={Messages.statsAreForSingles()} />;
  } else if (loading) {
    statsView = <LoadingScreen message={Messages.crunchingNumbers()} />;
  } else if (error) {
    statsView = <IconMessage Icon={ErrorIcon} label={`Error: ${error}`} />;
  } else if (stats) {
    statsView = <GameProfile file={file} stats={stats} onPlay={onPlay} />;
  } else {
    statsView = <IconMessage Icon={HelpIcon} label={Messages.noStatsComputed()} />;
  }

  return (
    <div className={styles.root}>
      <div className={styles.tabs}>
        <Tabs
          value={tab}
          onChange={(id) => setTab(id as Tab)}
          tabs={[
            { id: "stats", label: Messages.statsTab() },
            { id: "highlights", label: Messages.highlightsTab() },
          ]}
        />
      </div>
      {tab === "stats" ? statsView : <HighlightList filePath={filePath} pplusRelease={file.game.pplusRelease} />}
    </div>
  );
};
