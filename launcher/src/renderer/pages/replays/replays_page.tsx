import React from "react";
import { Navigate, Route, Routes, useParams } from "react-router-dom";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useReplayBrowserList, useReplayBrowserNavigation } from "@/lib/hooks/use_replay_browser_list";
import { useReplayPresenter, useReplays } from "@/lib/hooks/use_replays";
import { useSettings } from "@/lib/hooks/use_settings";
import { useServices } from "@/services";

import { ReplayBrowser } from "./replay_browser/replay_browser";
import { ReplayFileStats } from "./replay_file_stats/replay_file_stats";

export const ReplayBrowserPage = React.memo(() => {
  const { lastPath } = useReplayBrowserNavigation();
  const presenter = useReplayPresenter();

  // Opening Replays refreshes it (like its Refresh button) so the last game is listed at once. Stays mounted
  // between the list and a replay's stats, so going back to the list keeps its place. Skipped while a load is
  // already running: the app's start-up one, or the one Profile's replays button starts before coming here.
  React.useEffect(() => {
    if (useReplays.getState().loading) {
      return;
    }
    const { rootSlpPath, extraSlpPaths } = useSettings.getState().settings;
    presenter.init(rootSlpPath, extraSlpPaths, true, useReplays.getState().currentFolder).catch(() => undefined);
  }, [presenter]);

  return (
    <Routes>
      <Route path="list" element={<ReplayBrowser />} />
      <Route path=":filePath" element={<ChildPage />} />
      <Route path="*" element={<Navigate replace={true} to={lastPath} />} />
    </Routes>
  );
});

const ChildPage = () => {
  const { filePath } = useParams<Record<string, any>>();
  const selectedFile = useReplays((store) => store.selectedFile);
  const decodedFilePath = decodeURIComponent(filePath);
  const { dolphinService } = useServices();
  const { viewReplays } = useDolphinActions(dolphinService);
  const nav = useReplayBrowserList();
  const { goToReplayList } = useReplayBrowserNavigation();

  const onPlay = () => {
    void viewReplays({ path: decodedFilePath });
  };

  return (
    <ReplayFileStats
      filePath={decodedFilePath}
      file={selectedFile.fileResult}
      index={nav.index}
      total={nav.total}
      onNext={nav.selectNextFile}
      onPrev={nav.selectPrevFile}
      onClose={goToReplayList}
      onPlay={onPlay}
    />
  );
};
