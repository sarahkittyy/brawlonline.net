import React from "react";
import { Navigate, Outlet, Route, Routes, useLocation, useNavigate, useParams } from "react-router-dom";

import { useDolphinActions } from "@/lib/dolphin/use_dolphin_actions";
import { useReplayBrowserList, useReplayBrowserNavigation } from "@/lib/hooks/use_replay_browser_list";
import { useReplayPresenter, useReplays } from "@/lib/hooks/use_replays";
import { useSettings } from "@/lib/hooks/use_settings";
import { Tabs } from "@/pages/home/tabs/tabs";
import { useServices } from "@/services";

import { AllHighlightsPage } from "./all_highlights/all_highlights_page";
import { ReplayBrowser } from "./replay_browser/replay_browser";
import { ReplayBrowserMessages } from "./replay_browser/replay_browser.messages";
import { ReplayFileStats } from "./replay_file_stats/replay_file_stats";

export const ReplayBrowserPage = React.memo(() => {
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
      <Route element={<ReplaysTabs />}>
        <Route path="list" element={<ReplayBrowser />} />
        <Route path="highlights" element={<AllHighlightsPage />} />
      </Route>
      <Route path=":filePath" element={<ChildPage />} />
      {/* (The header's Replays button always opens the Replays tab.) */}
      <Route path="*" element={<Navigate replace={true} to="list" />} />
    </Routes>
  );
});

const REPLAY_TABS = ["list", "highlights"] as const;

/** Replays | Highlights, as Home's tabs. */
const ReplaysTabs = () => {
  const navigate = useNavigate();
  const { pathname } = useLocation();
  const current = REPLAY_TABS.find((tab) => pathname.endsWith(`/replays/${tab}`)) ?? "list";
  return (
    <div style={{ display: "flex", flexDirection: "column", flex: 1, minHeight: 0, minWidth: 0 }}>
      {/* (A gap below, so the tab's underline doesn't meet the folder list's selected row.) */}
      <div style={{ paddingBottom: 6 }}>
        <Tabs
          value={current}
          onChange={(tab) => navigate(`/main/replays/${tab}`)}
          tabs={[
            { id: "list", label: ReplayBrowserMessages.replaysTab() },
            { id: "highlights", label: ReplayBrowserMessages.highlightsTab() },
          ]}
        />
      </div>
      <Outlet />
    </div>
  );
};

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
